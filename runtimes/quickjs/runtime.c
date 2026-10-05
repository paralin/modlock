// The QuickJS runtime runs JavaScript mods. It is a WASI reactor with the same
// boundary as a compiled mod: the host hands the start event, which carries
// the mod's bundle in StartEvent.source, and the runtime evaluates the bundle
// once. Every event, the start event included, then goes to the function the
// bundle stores in __modlock.event, which returns the encoded Reply.
//
// The runtime adds what the bundle needs and QuickJS lacks: __modlock with the
// host call, a log line and UTF-8 conversion, then TextEncoder, TextDecoder and
// console built on them.

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
