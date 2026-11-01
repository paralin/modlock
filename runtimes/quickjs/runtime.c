// The QuickJS runtime runs JavaScript mods. It is a WASI reactor with the same
// boundary as a compiled mod: the host hands the start event, which carries
// the mod's bundle in StartEvent.source, and the runtime evaluates the bundle
// once. Every event, the start event included, then goes to the function the
// bundle stores in __modlock.event, which returns the encoded Reply.
//
// The runtime adds what the bundle needs and QuickJS lacks: __modlock with the
// host call, a log line and UTF-8 conversion, then TextEncoder, TextDecoder and
// console built on them.
//
// It also samples the bundle's call stack for the host. While a call runs
// slow, the host sets sample_due about every 10 ms. QuickJS polls its interrupt
// handler every few thousand operations, and the handler takes the stack when
// it finds sample_due set, so a call that is not sampled pays one read of a
// word per poll. The host takes the counted stacks with modlock_samples.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../boundary.h"
#include "quickjs.h"

// The bundle runs in one context for the instance's lifetime; a reload starts
// a new instance.
static JSRuntime* runtime;
static JSContext* context;

// result holds the last encoded Reply until the next event.
static uint8_t* result;

// sample_due is set by the host, at the address modlock_sample_flag returns,
// when the running call is due a stack sample, and cleared here once the sample
// is taken.
static volatile uint32_t sample_due;

// modlock_sample_flag returns the address of the word the host sets to ask for
// a stack sample.
__attribute__((export_name("modlock_sample_flag"))) uint32_t modlock_sample_flag(void) {
  return (uint32_t)(uintptr_t)&sample_due;
}

// prelude defines the globals a bundle expects on top of __modlock.
static const char prelude[] =
    "(() => {\n"
    "  const host = globalThis.__modlock;\n"
    "  globalThis.TextEncoder = class TextEncoder {\n"
    "    get encoding() { return 'utf-8'; }\n"
    "    encode(text = '') { return host.encodeUtf8(String(text)); }\n"
    "  };\n"
    "  globalThis.TextDecoder = class TextDecoder {\n"
    "    get encoding() { return 'utf-8'; }\n"
    "    decode(bytes) {\n"
    "      if (bytes === undefined) return '';\n"
    "      if (ArrayBuffer.isView(bytes)) {\n"
    "        bytes = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);\n"
    "      } else {\n"
    "        bytes = new Uint8Array(bytes);\n"
    "      }\n"
    "      return host.decodeUtf8(bytes);\n"
    "    }\n"
    "  };\n"
    "  const show = (part) => {\n"
    "    if (typeof part === 'string') return part;\n"
    "    if (part instanceof Error) return part.stack ? `${part}\\n${part.stack}` : String(part);\n"
    "    try { return JSON.stringify(part) ?? String(part); } catch { return String(part); }\n"
    "  };\n"
    "  const print = (...parts) => host.log(parts.map(show).join(' '));\n"
    "  globalThis.console = { log: print, info: print, warn: print, error: print, debug: print };\n"
    "})();\n";

// log_exception logs the pending exception with its stack.
static void log_exception(const char* what) {
  JSValue exception = JS_GetException(context);
  JSValue stack = JS_GetPropertyStr(context, exception, "stack");
  const char* message = JS_ToCString(context, exception);
  const char* trace = JS_IsUndefined(stack) ? NULL : JS_ToCString(context, stack);

  size_t size =
      strlen(what) + 2 + (message ? strlen(message) : 0) + 1 + (trace ? strlen(trace) : 0) + 1;
  char* line = malloc(size);
  if (!line) __builtin_trap();
  size_t written = (size_t)snprintf(line, size, "%s: %s%s%s", what, message ? message : "?",
                                    trace ? "\n" : "", trace ? trace : "");
  host_log(line, written < size ? written : size - 1);

  free(line);
  JS_FreeCString(context, trace);
  JS_FreeCString(context, message);
  JS_FreeValue(context, stack);
  JS_FreeValue(context, exception);
}

// js_host_call is __modlock.hostCall(request: Uint8Array): Uint8Array.
static JSValue js_host_call(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
  size_t size;
  uint8_t* request = JS_GetUint8Array(ctx, &size, argv[0]);
  if (!request) return JS_EXCEPTION;
  uint32_t response_size;
  uint8_t* response = exchange(request, size, &response_size);
  JSValue bytes = JS_NewUint8ArrayCopy(ctx, response, response_size);
  free(response);
  return bytes;
}

// js_log is __modlock.log(text: string).
static JSValue js_log(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
  size_t size;
  const char* text = JS_ToCStringLen(ctx, &size, argv[0]);
  if (!text) return JS_EXCEPTION;
  host_log(text, size);
  JS_FreeCString(ctx, text);
  return JS_UNDEFINED;
}

// js_encode_utf8 is __modlock.encodeUtf8(text: string): Uint8Array.
static JSValue js_encode_utf8(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
  size_t size;
  const char* text = JS_ToCStringLen(ctx, &size, argv[0]);
  if (!text) return JS_EXCEPTION;
  JSValue bytes = JS_NewUint8ArrayCopy(ctx, (const uint8_t*)text, size);
  JS_FreeCString(ctx, text);
  return bytes;
}

// js_decode_utf8 is __modlock.decodeUtf8(bytes: Uint8Array): string.
static JSValue js_decode_utf8(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
  size_t size;
  uint8_t* bytes = JS_GetUint8Array(ctx, &size, argv[0]);
  if (!bytes) return JS_EXCEPTION;
  return JS_NewStringLen(ctx, (const char*)bytes, size);
}

// host_functions are __modlock's native members.
static const JSCFunctionListEntry host_functions[] = {
    JS_CFUNC_DEF("hostCall", 1, js_host_call),
    JS_CFUNC_DEF("log", 1, js_log),
    JS_CFUNC_DEF("encodeUtf8", 1, js_encode_utf8),
    JS_CFUNC_DEF("decodeUtf8", 1, js_decode_utf8),
};

// A stack sample is the top kSampleLines lines of QuickJS's backtrace, kept
// as text. The table counts distinct samples; one past its capacity is only
// counted in sample_total.
enum { kSampleLines = 8, kSampleBytes = 1024, kSampleStacks = 16 };

static struct {
  char text[kSampleBytes];
  uint32_t size;
  uint32_t count;
} sample_stacks[kSampleStacks];
static uint32_t sample_stack_count;
static uint32_t sample_total;

// record_sample counts one backtrace, keeping its first kSampleLines lines.
static void record_sample(const char* trace) {
  // Cut the trace after kSampleLines lines or kSampleBytes bytes, at a line.
  size_t size = 0;
  size_t cut = 0;
  int lines = 0;
  while (trace[size] && lines < kSampleLines && size < kSampleBytes) {
    if (trace[size++] == '\n') {
      cut = size;
      ++lines;
    }
  }
  if (lines < kSampleLines && !trace[size]) cut = size;
  if (cut == 0) return;
  ++sample_total;

  // Count it under the stack it repeats, or a new one while the table has room.
  for (uint32_t i = 0; i < sample_stack_count; ++i) {
    if (sample_stacks[i].size == cut && memcmp(sample_stacks[i].text, trace, cut) == 0) {
      ++sample_stacks[i].count;
      return;
    }
  }
  if (sample_stack_count == kSampleStacks) return;
  memcpy(sample_stacks[sample_stack_count].text, trace, cut);
  sample_stacks[sample_stack_count].size = (uint32_t)cut;
  sample_stacks[sample_stack_count].count = 1;
  ++sample_stack_count;
}

// sample_interrupt is QuickJS's interrupt handler. It never interrupts the
// bundle, and takes a sample when the host asked for one.
static int sample_interrupt(JSRuntime* rt, void* opaque) {
  if (!sample_due) return 0;
  sample_due = 0;
  JSValue error = JS_NewError(context);
  if (JS_IsException(error)) {
    JS_FreeValue(context, JS_GetException(context));
    return 0;
  }
  JSValue stack = JS_GetPropertyStr(context, error, "stack");
  const char* trace = JS_IsException(stack) ? NULL : JS_ToCString(context, stack);
  if (trace) {
    record_sample(trace);
    JS_FreeCString(context, trace);
  } else {
    JS_FreeValue(context, JS_GetException(context));
  }
  JS_FreeValue(context, stack);
  JS_FreeValue(context, error);
  return 0;
}

// evaluate runs code as a global script named file and reports whether it
// completed.
static int evaluate(const uint8_t* code, size_t size, const char* file) {
  // QuickJS reads one byte past the end, which must be zero.
  char* text = malloc(size + 1);
  if (!text) __builtin_trap();
  memcpy(text, code, size);
  text[size] = 0;
  JSValue value = JS_Eval(context, text, size, file, JS_EVAL_TYPE_GLOBAL);
  free(text);
  if (JS_IsException(value)) {
    log_exception(file);
    return 0;
  }
  JS_FreeValue(context, value);
  return 1;
}

// start creates the context and evaluates the bundle in the start event.
static int start(const uint8_t* event, size_t size) {
  const uint8_t* source;
  size_t source_size;
  if (!start_source(event, size, &source, &source_size)) {
    static const char missing[] = "the start event carries no JavaScript source";
    host_log(missing, sizeof(missing) - 1);
    return 0;
  }

  // Build the globals, then run the bundle.
  runtime = JS_NewRuntime();
  if (!runtime) return 0;
  JS_SetInterruptHandler(runtime, sample_interrupt, NULL);
  context = JS_NewContext(runtime);
  if (!context) return 0;
  JSValue global = JS_GetGlobalObject(context);
  JSValue host = JS_NewObject(context);
  JS_SetPropertyFunctionList(context, host, host_functions,
                             sizeof(host_functions) / sizeof(host_functions[0]));
  JS_SetPropertyStr(context, global, "__modlock", host);
  JS_FreeValue(context, global);
  return evaluate((const uint8_t*)prelude, sizeof(prelude) - 1, "prelude.js") &&
         evaluate(source, source_size, "mod.js");
}

// run_jobs settles the promises the last call left pending.
static void run_jobs(void) {
  JSContext* job_context;
  int ran;
  while ((ran = JS_ExecutePendingJob(runtime, &job_context)) != 0) {
    if (ran < 0) log_exception("promise job");
  }
}

// modlock_event receives one encoded Call of size bytes, hands it to the
// bundle, and returns the encoded Reply's address in the high 32 bits and its
// length in the low 32 bits. A bundle that fails to start traps, which stops
// the mod.
__attribute__((export_name("modlock_event"))) uint64_t modlock_event(uint32_t size) {
  // Copy the event out of the host.
  uint8_t* event = malloc(size ? size : 1);
  if (!event) __builtin_trap();
  host_read(event, size);
  int starting = !context;
  if (starting && !start(event, size)) __builtin_trap();

  // Hand it to the bundle's handler.
  JSValue global = JS_GetGlobalObject(context);
  JSValue host = JS_GetPropertyStr(context, global, "__modlock");
  JSValue handler = JS_GetPropertyStr(context, host, "event");
  JSValue answer = JS_UNDEFINED;
  if (JS_IsFunction(context, handler)) {
    JSValue bytes = JS_NewUint8ArrayCopy(context, event, size);
    answer = JS_Call(context, handler, host, 1, &bytes);
    JS_FreeValue(context, bytes);
  } else if (starting) {
    static const char missing[] = "the bundle set no __modlock.event handler";
    host_log(missing, sizeof(missing) - 1);
  }
  free(event);
  JS_FreeValue(context, handler);
  JS_FreeValue(context, host);
  JS_FreeValue(context, global);
  if (JS_IsException(answer)) {
    log_exception("event");
    if (starting) __builtin_trap();
    answer = JS_UNDEFINED;
  }
  run_jobs();

  // Keep the encoded answer until the next event.
  free(result);
  result = NULL;
  size_t answer_size = 0;
  uint8_t* bytes = NULL;
  if (!JS_IsUndefined(answer) && !(bytes = JS_GetUint8Array(context, &answer_size, answer))) {
    log_exception("the event handler must return a Uint8Array");
  }
  if (bytes && answer_size) {
    result = malloc(answer_size);
    if (!result) __builtin_trap();
    memcpy(result, bytes, answer_size);
  }
  JS_FreeValue(context, answer);
  if (!result) return 0;
  return (uint64_t)(uintptr_t)result << 32 | answer_size;
}

// kReportBytes bounds the encoded report: a stack takes under 1.5 KiB.
enum { kReportBytes = 32 * 1024, kStackBytes = 2048, kFrameBytes = 1280 };

// put_field appends a length-delimited field to out at *at.
static void put_field(uint8_t* out, size_t* at, uint8_t tag, const uint8_t* data, size_t size) {
  out[(*at)++] = tag;
  *at += put_varint(out + *at, size);
  memcpy(out + *at, data, size);
  *at += size;
}

// put_number appends a varint field to out at *at.
static void put_number(uint8_t* out, size_t* at, uint8_t tag, uint64_t value) {
  out[(*at)++] = tag;
  *at += put_varint(out + *at, value);
}

// encode_frame encodes the StackFrame in one backtrace line, such as
// "    at update (mod.js:12:5)", "    at <anonymous> (missing)" or
// "    at call (native)", into out, and returns its length.
static size_t encode_frame(const char* line, size_t size, uint8_t* out) {
  // Split the function from the position in the parentheses.
  while (size && line[size - 1] == '\n') --size;
  if (size > 7 && memcmp(line, "    at ", 7) == 0) {
    line += 7;
    size -= 7;
  }
  const char* function = line;
  size_t function_size = size;
  const char* where = NULL;
  size_t where_size = 0;
  for (size_t i = 0; i + 1 < size; ++i) {
    if (line[i] == ' ' && line[i + 1] == '(') {
      function_size = i;
      where = line + i + 2;
      where_size = size - i - 2;
      if (where_size && where[where_size - 1] == ')') --where_size;
      break;
    }
  }
  if (function_size == 11 && memcmp(function, "<anonymous>", 11) == 0) function_size = 0;

  // Read ":line:column" off the end of the position, leaving the file.
  uint64_t numbers[2] = {0, 0};
  for (int n = 1; n >= 0 && where_size; --n) {
    size_t digits = 0;
    while (digits < where_size && where[where_size - 1 - digits] >= '0' &&
           where[where_size - 1 - digits] <= '9') {
      ++digits;
    }
    if (digits == 0 || digits == where_size || where[where_size - 1 - digits] != ':') {
      numbers[0] = numbers[1] = 0;
      break;
    }
    uint64_t value = 0;
    for (size_t d = where_size - digits; d < where_size; ++d) value = value * 10 + (where[d] - '0');
    numbers[n] = value;
    where_size -= digits + 1;
  }
  if ((where_size == 7 && memcmp(where, "missing", 7) == 0) ||
      (where_size == 6 && memcmp(where, "native", 6) == 0)) {
    where_size = 0;
  }

  size_t at = 0;
  if (function_size) put_field(out, &at, 0x0a, (const uint8_t*)function, function_size);
  if (where_size) put_field(out, &at, 0x12, (const uint8_t*)where, where_size);
  if (numbers[0]) put_number(out, &at, 0x18, numbers[0]);
  if (numbers[1]) put_number(out, &at, 0x20, numbers[1]);
  return at;
}

// modlock_samples returns the stacks sampled since the last call as an encoded
// modlock.wasm.profile.StackSamples, most sampled first, with its address in
// the high 32 bits and its length in the low 32 bits, and starts counting
// afresh. It returns 0 when nothing was sampled.
__attribute__((export_name("modlock_samples"))) uint64_t modlock_samples(void) {
  static uint8_t report[kReportBytes];
  if (sample_total == 0) return 0;
  size_t at = 0;
  put_number(report, &at, 0x08, sample_total);

  // Emit the stacks by falling count.
  uint8_t done[kSampleStacks] = {0};
  for (uint32_t emitted = 0; emitted < sample_stack_count; ++emitted) {
    uint32_t best = kSampleStacks;
    for (uint32_t i = 0; i < sample_stack_count; ++i) {
      if (!done[i] && (best == kSampleStacks || sample_stacks[i].count > sample_stacks[best].count)) {
        best = i;
      }
    }
    done[best] = 1;

    // Encode its lines as frames, then the stack around them.
    static uint8_t stack[kStackBytes];
    static uint8_t frame[kFrameBytes];
    size_t stack_at = 0;
    const char* text = sample_stacks[best].text;
    size_t size = sample_stacks[best].size;
    for (size_t start = 0; start < size;) {
      size_t end = start;
      while (end < size && text[end] != '\n') ++end;
      size_t frame_size = encode_frame(text + start, end - start, frame);
      if (stack_at + frame_size + 6 <= kStackBytes) {
        put_field(stack, &stack_at, 0x0a, frame, frame_size);
      }
      start = end + 1;
    }
    put_number(stack, &stack_at, 0x10, sample_stacks[best].count);
    if (at + stack_at + 6 <= kReportBytes) put_field(report, &at, 0x12, stack, stack_at);
  }

  memset(sample_stacks, 0, sizeof(sample_stacks[0]) * sample_stack_count);
  sample_stack_count = 0;
  sample_total = 0;
  return (uint64_t)(uintptr_t)report << 32 | at;
}
