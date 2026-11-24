//go:build deps_only

#include "wasm/instance.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <variant>

#include <wasmtime/async.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace modlock::wasm {
namespace {

// ThreadTime returns the processor time the calling thread has used, in user
// and kernel mode. Time it spends blocked, preempted or waiting for memory
// paged in from disk is not in it.
std::chrono::nanoseconds ThreadTime() {
#if defined(_WIN32)
  // Read the thread's processor time.
  FILETIME creation, exit, kernel, user;
  GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
  auto hundreds = [](FILETIME time) {
    return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
  };
  return std::chrono::nanoseconds{(hundreds(kernel) + hundreds(user)) * 100};
#else
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return std::chrono::seconds{now.tv_sec} + std::chrono::nanoseconds{now.tv_nsec};
#endif
}

// Bytes returns the mod memory range [data, data + size), or nullopt when the
// range leaves memory.
std::optional<std::span<uint8_t>> Bytes(wasmtime::Span<uint8_t> memory, uint32_t data,
                                        uint32_t size) {
  if (uint64_t{data} + size > memory.size()) {
    return std::nullopt;
  }
  return std::span<uint8_t>(memory.data() + data, size);
}

// CallerBytes resolves a range of the calling mod's exported memory.
std::optional<std::span<uint8_t>> CallerBytes(wasmtime::Caller& caller, uint32_t data,
                                              uint32_t size) {
  // Find the caller's exported memory.
  auto exported = caller.get_export("memory");
  if (!exported) {
    return std::nullopt;
  }
  auto* memory = std::get_if<wasmtime::Memory>(&*exported);
  if (!memory) {
    return std::nullopt;
  }
  return Bytes(memory->data(caller.context()), data, size);
}

#if defined(_WIN32)
// StackProbe measures how deep a host call reaches on the fiber that runs a
// sliced call. With MODLOCK_STACK_PROBE set, StackPaint fills the stack below
// the caller with a pattern, and StackReport finds how far the call
// overwrote it. The first paint also raises a structured exception on the
// fiber, to show that exception dispatch works there.
struct StackProbe {
  std::string method;
  ULONG_PTR low = 0, high = 0;
  uintptr_t top = 0, floor = 0;
  bool painted = false;
};

constexpr uint8_t kStackPattern = 0xA5;

LONG CALLBACK ContinueRaised(EXCEPTION_POINTERS* info) {
  return info->ExceptionRecord->ExceptionCode == 0x20000001 ? EXCEPTION_CONTINUE_EXECUTION
                                                            : EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((noinline)) void FiberExceptions() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  void* handler = AddVectoredExceptionHandler(1, ContinueRaised);
  RaiseException(0x20000001, 0, 0, nullptr);
  RemoveVectoredExceptionHandler(handler);
  std::fprintf(stderr,
               "[stackprobe] on the fiber: a raised exception reached its handler and resumed\n");
}

__attribute__((noinline)) StackProbe StackPaint(const std::string& method, bool fiber) {
  static const bool enabled = std::getenv("MODLOCK_STACK_PROBE") != nullptr;
  static std::map<std::string, int> seen;
  StackProbe probe;
  if (!enabled || !fiber || ++seen[method] > 3) {
    return probe;
  }
  FiberExceptions();
  probe.method = method;
  GetCurrentThreadStackLimits(&probe.low, &probe.high);
  volatile char marker = 0;
  probe.top = (reinterpret_cast<uintptr_t>(&marker) - 2048) & ~uintptr_t{4095};
  // Paint page by page downward, so the guard page grows the stack as it does
  // for the call's own probes.
  constexpr uintptr_t kPaint = uintptr_t{6} << 20;
  constexpr uintptr_t kMargin = 32 << 10;
  const uintptr_t floor =
      std::max<uintptr_t>(probe.low + kMargin, probe.top - std::min(probe.top - probe.low, kPaint));
  probe.floor = (floor + 4095) & ~uintptr_t{4095};
  for (uintptr_t page = probe.top; page >= probe.floor; page -= 4096) {
    std::memset(reinterpret_cast<void*>(page), kStackPattern, 4096);
  }
  probe.painted = true;
  return probe;
}

__attribute__((noinline)) void StackReport(const StackProbe& probe) {
  if (!probe.painted) {
    return;
  }
  // The lowest byte the call changed is the deepest it went.
  const auto* bytes = reinterpret_cast<const uint8_t*>(probe.floor);
  const size_t length = probe.top + 4096 - probe.floor;
  size_t i = 0;
  while (i < length && bytes[i] == kStackPattern) {
    ++i;
  }
  std::fprintf(stderr,
               "[stackprobe] %s: reached %zu bytes below the host call (the call's frame is %zu "
               "bytes below the fiber top; the fiber holds %zu bytes; %zu painted)%s\n",
               probe.method.c_str(), static_cast<size_t>(probe.top + 4096 - (probe.floor + i)),
               static_cast<size_t>(probe.high - probe.top),
               static_cast<size_t>(probe.high - probe.low), length,
               i == 0 ? " REACHED THE PAINT FLOOR" : "");
}
#endif

}  // namespace

// ParkedCall is a sliced call in flight. Wasmtime borrows args, results and
// the trap and error slots until the future is deleted, so they live here.
struct Instance::ParkedCall {
  ~ParkedCall() {
    if (future) {
      wasmtime_call_future_delete(future);
    }
    if (trap) {
      wasm_trap_delete(trap);
    }
    if (error) {
      wasmtime_error_delete(error);
    }
  }
  wasmtime_call_future_t* future = nullptr;
  wasmtime_val_t args[1]{};
  wasmtime_val_t results[1]{};
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* error = nullptr;
};

Instance::Instance(Runtime& runtime, const Limits& limits, HostCall host_call)
    : runtime_(runtime),
      limits_(limits),
      host_call_(std::move(host_call)),
      store_(runtime.Engine()) {
  store_.limiter(limits_.memory_bytes, -1, -1, -1, -1);

  // The epoch deadline arrives no sooner than the budget can be spent, since
  // the thread uses at most one second of processor time per second. A call
  // with budget left runs on to a new deadline; one without traps.
  // A call past limits_.sample_after also gets a callback on every tick, which
  // samples it.
  store_.epoch_deadline_callback(
      [this](wasmtime::Store::Context context,
             uint64_t& delta) -> wasmtime::Result<wasmtime::DeadlineKind> {
        if (Left() <= Left().zero()) {
          return wasmtime::Error("the time budget is spent");
        }
        Sample(context);
        // A sliced call past its slice's end yields to the next tick.
        if (slice_end_ && std::chrono::steady_clock::now() >= *slice_end_) {
          delta = 1;
          return wasmtime::DeadlineKind::Yield;
        }
        delta = NextTicks();
        return wasmtime::DeadlineKind::Continue;
      });
}

Instance::~Instance() = default;

std::expected<std::unique_ptr<Instance>, std::string> Instance::Load(
    Runtime& runtime, std::span<const uint8_t> module, const Limits& limits, HostCall host_call) {
  std::unique_ptr<Instance> instance(new Instance(runtime, limits, std::move(host_call)));
  if (auto instantiated = instance->Instantiate(module); !instantiated) {
    return std::unexpected(instantiated.error());
  }
  return instance;
}

std::expected<void, std::string> Instance::Instantiate(std::span<const uint8_t> module) {
  // Compile the module, or share the copy another instance compiled.
  auto compiled = runtime_.Compile(module);
  if (!compiled) {
    return std::unexpected(compiled.error());
  }
  module_ = std::move(*compiled);

  // Give the mod standard output and error and nothing else from WASI.
  wasmtime::WasiConfig wasi;
  wasi.inherit_stdout();
  wasi.inherit_stderr();
  if (auto set = store_.context().set_wasi(std::move(wasi)); !set) {
    return std::unexpected("cannot configure WASI: " + set.err().message());
  }

  // Link WASI and the modlock imports, then instantiate.
  wasmtime::Linker linker(runtime_.Engine());
  if (auto defined = linker.define_wasi(); !defined) {
    return std::unexpected("cannot define WASI: " + defined.err().message());
  }
  if (auto defined = DefineImports(linker); !defined) {
    return defined;
  }
  SetBudget("initialization", limits_.start_budget);
  auto instance = linker.instantiate(store_.context(), *module_);
  if (!instance) {
    return std::unexpected("cannot instantiate the mod: " + instance.err().message());
  }

  // Find the memory and the event entry point the boundary needs.
  auto context = store_.context();
  auto memory = instance.ok_ref().get(context, "memory");
  if (memory) {
    if (auto* exported = std::get_if<wasmtime::Memory>(&*memory)) {
      memory_ = *exported;
    }
  }
  auto event = instance.ok_ref().get(context, "modlock_event");
  if (event) {
    if (auto* exported = std::get_if<wasmtime::Func>(&*event)) {
      event_ = *exported;
    }
  }
  if (!memory_ || !event_) {
    return std::unexpected("the mod must export memory and modlock_event");
  }

  // Run the reactor's initialization, which registers the mod's handlers.
  busy_ = true;
  auto initialize = instance.ok_ref().get(context, "_initialize");
  if (auto* function = initialize ? std::get_if<wasmtime::Func>(&*initialize) : nullptr) {
    auto initialized = function->call(context, {});
    if (!initialized) {
      busy_ = false;
      return std::unexpected(Stop(initialized.err().message()));
    }
  }

  // An interpreter samples its own stack, so ask where it reads the request.
  auto samples = instance.ok_ref().get(context, "modlock_samples");
  auto flag = instance.ok_ref().get(context, "modlock_sample_flag");
  auto* samples_func = samples ? std::get_if<wasmtime::Func>(&*samples) : nullptr;
  auto* flag_func = flag ? std::get_if<wasmtime::Func>(&*flag) : nullptr;
  if (samples_func && flag_func) {
    auto typed = flag_func->typed<std::tuple<>, uint32_t>(context);
    if (typed) {
      if (auto address = typed.ok_ref().call(context, {}); address) {
        sample_flag_ = address.ok();
        samples_func_ = *samples_func;
      }
    }
  }
  busy_ = false;
  FinishSamples();
  return {};
}

std::expected<void, std::string> Instance::DefineImports(wasmtime::Linker& linker) {
  // Define the host call and host read imports.
  auto call = linker.func_wrap("modlock", "host_call",
                               [this](wasmtime::Caller caller, uint32_t data, uint32_t size) {
                                 return HostCallImport(caller, data, size);
                               });
  if (!call) {
    return std::unexpected("cannot define host_call: " + call.err().message());
  }
  auto read = linker.func_wrap("modlock", "host_read",
                               [this](wasmtime::Caller caller, uint32_t data, uint32_t size) {
                                 return HostReadImport(caller, data, size);
                               });
  if (!read) {
    return std::unexpected("cannot define host_read: " + read.err().message());
  }
  return {};
}

std::expected<Reply, std::string> Instance::Deliver(const Call& call) {
  // Handle one event at a time, and none after a failure.
  if (failure_) {
    return std::unexpected(*failure_);
  }
  if (busy_) {
    return std::unexpected("the mod is already handling an event");
  }

  // Call the mod, which copies the call out with host_read.
  auto typed = event_->typed<uint32_t, uint64_t>(store_.context());
  if (!typed) {
    return std::unexpected(Fail("modlock_event must take i32 and return i64"));
  }

  // Pass the call, timing the mod within its budget.
  call.SerializeToString(&pending_);
  SetBudget(call.method(), call.method() == "Start" ? limits_.start_budget : limits_.event_budget);
  busy_ = true;
  auto called = typed.ok_ref().call(store_.context(), static_cast<uint32_t>(pending_.size()));
  busy_ = false;
  budget_.spent = ThreadTime() - budget_.began;
  pending_.clear();
  if (!called) {
    return std::unexpected(Stop(called.err().message()));
  }
  FinishSamples();
  return Decode(called.ok());
}

std::expected<std::optional<Reply>, std::string> Instance::DeliverSliced(
    const Call& call, std::chrono::steady_clock::time_point slice_end) {
  // Handle one event at a time, and none after a failure.
  if (failure_) {
    return std::unexpected(*failure_);
  }
  if (busy_) {
    return std::unexpected("the mod is already handling an event");
  }

  // Start the call on a fiber, timing the mod within its budget.
  call.SerializeToString(&pending_);
  slice_end_ = slice_end;
  SetBudget(call.method(), limits_.event_budget);
  busy_ = true;
  parked_ = std::make_unique<ParkedCall>();
  parked_->args[0].kind = WASMTIME_I32;
  parked_->args[0].of.i32 = static_cast<int32_t>(pending_.size());
  parked_->future =
      wasmtime_func_call_async(store_.context().capi(), &event_->capi(), parked_->args, 1,
                               parked_->results, 1, &parked_->trap, &parked_->error);
  return Poll(slice_end);
}

std::expected<std::optional<Reply>, std::string> Instance::Resume(
    std::chrono::steady_clock::time_point slice_end) {
  if (!parked_) {
    return std::unexpected("no call is parked");
  }
  // The thread's time while the call was parked is not the call's.
  const auto away = ThreadTime() - parked_at_;
  budget_.began += away;
  last_sample_ += away;
  return Poll(slice_end);
}

std::expected<std::optional<Reply>, std::string> Instance::Poll(
    std::chrono::steady_clock::time_point slice_end) {
  // Run the call until it returns or yields at the slice's end.
  slice_end_ = slice_end;
  ArmDeadline();
  const bool done = wasmtime_call_future_poll(parked_->future);
  slice_end_.reset();
  if (!done) {
    parked_at_ = ThreadTime();
    return std::nullopt;
  }

  // The call returned: free the future and read its result.
  auto parked = std::move(parked_);
  busy_ = false;
  budget_.spent = ThreadTime() - budget_.began;
  pending_.clear();
  if (parked->trap) {
    return std::unexpected(Stop(wasmtime::Trap(std::exchange(parked->trap, nullptr)).message()));
  }
  if (parked->error) {
    return std::unexpected(Stop(wasmtime::Error(std::exchange(parked->error, nullptr)).message()));
  }
  FinishSamples();
  auto reply = Decode(static_cast<uint64_t>(parked->results[0].of.i64));
  if (!reply) {
    return std::unexpected(reply.error());
  }
  return std::optional<Reply>(std::move(*reply));
}

std::expected<Reply, std::string> Instance::Decode(uint64_t packed) {
  // Decode the reply the mod left in its memory.
  Reply reply;
  if (packed == 0) {
    return reply;
  }
  auto bytes = Bytes(memory_->data(store_.context()), static_cast<uint32_t>(packed >> 32),
                     static_cast<uint32_t>(packed));
  if (!bytes) {
    return std::unexpected(Fail("modlock_event returned a reply outside memory"));
  }
  if (!reply.ParseFromArray(bytes->data(), static_cast<int>(bytes->size()))) {
    return std::unexpected(Fail("modlock_event returned an invalid Reply"));
  }
  return reply;
}

wasmtime::Result<uint32_t, wasmtime::Trap> Instance::HostCallImport(wasmtime::Caller caller,
                                                                    uint32_t data, uint32_t size) {
  // Decode the call from mod memory.
  auto bytes = CallerBytes(caller, data, size);
  if (!bytes) {
    return wasmtime::Trap("modlock.host_call: the call is outside memory");
  }
  Call call;
  if (!call.ParseFromArray(bytes->data(), static_cast<int>(bytes->size()))) {
    return wasmtime::Trap("modlock.host_call: invalid Call");
  }

  // Answer it and hold the encoded reply for host_read. The host's work
  // counts against the budget, in thread time; past it, the mod traps on
  // return. The report times each call on the steady clock instead, since
  // Wine answers thread time in 10 ms steps that round most calls to 0 or 10.
  const auto entered = std::chrono::steady_clock::now();
#if defined(_WIN32)
  const auto stack = StackPaint(call.method(), parked_ != nullptr);
#endif
  host_call_(call).SerializeToString(&pending_);
#if defined(_WIN32)
  StackReport(stack);
#endif
  const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - entered);
  budget_.host += spent;
  if (spent > budget_.slowest_time) {
    budget_.slowest = call.method();
    budget_.slowest_time = spent;
  }
  ArmDeadline();
  return static_cast<uint32_t>(pending_.size());
}

wasmtime::Result<std::monostate, wasmtime::Trap> Instance::HostReadImport(wasmtime::Caller caller,
                                                                          uint32_t data,
                                                                          uint32_t size) {
  // Copy the pending call into the mod's buffer.
  if (size != pending_.size()) {
    return wasmtime::Trap("modlock.host_read: the size differs from the pending message");
  }
  auto bytes = CallerBytes(caller, data, size);
  if (!bytes) {
    return wasmtime::Trap("modlock.host_read: the buffer is outside memory");
  }
  std::memcpy(bytes->data(), pending_.data(), size);
  pending_.clear();
  return std::monostate{};
}

void Instance::SetBudget(std::string call, std::chrono::milliseconds limit) {
  budget_ = {.call = std::move(call), .limit = limit, .began = ThreadTime()};
  sampled_ = false;
  guest_.reset();
  last_sample_ = budget_.began;
  samples_.Clear();
  ArmDeadline();
}

std::chrono::nanoseconds Instance::Left() const {
  return budget_.limit - (ThreadTime() - budget_.began);
}

uint64_t Instance::Ticks(std::chrono::nanoseconds left) {
  if (left <= left.zero()) {
    return 0;
  }
  return static_cast<uint64_t>((left + kEpochTick - std::chrono::nanoseconds{1}) / kEpochTick);
}

uint64_t Instance::NextTicks() const {
  // Wake for the sample when the call is not due one yet, and on every tick
  // once it is.
  auto ticks = Ticks(Left());
  if (slice_end_) {
    ticks = std::min(ticks, Ticks(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   *slice_end_ - std::chrono::steady_clock::now())));
  }
  if (limits_.sample_after <= limits_.sample_after.zero()) {
    return ticks;
  }
  const auto until = limits_.sample_after - (ThreadTime() - budget_.began);
  return std::min(ticks, std::max<uint64_t>(Ticks(until), Ticks(kSampleTick)));
}

void Instance::ArmDeadline() {
  store_.context().set_epoch_deadline(NextTicks());
}

void Instance::Sample(wasmtime::Store::Context context) {
  const auto now = ThreadTime();
  if (limits_.sample_after <= limits_.sample_after.zero() ||
      now - budget_.began < limits_.sample_after) {
    return;
  }
  sampled_ = true;

  // Ask an interpreter for its own stack at its next poll.
  if (sample_flag_) {
    if (auto flag = Bytes(memory_->data(context), *sample_flag_, 4)) {
      const uint32_t due = 1;
      std::memcpy(flag->data(), &due, sizeof(due));
    }
    return;
  }

  // Record the Wasm stack of any other module.
  if (!guest_) {
    guest_ = std::make_unique<GuestSampler>(runtime_.Engine(), *module_);
  }
  guest_->Sample(store_, now - last_sample_);
  last_sample_ = now;
}

void Instance::FinishSamples() {
  if (!sampled_) {
    return;
  }
  sampled_ = false;
  StackTally tally;

  // Take an interpreter's counted stacks, and clear its request.
  if (sample_flag_) {
    if (auto flag = Bytes(memory_->data(store_.context()), *sample_flag_, 4)) {
      const uint32_t none = 0;
      std::memcpy(flag->data(), &none, sizeof(none));
    }
    auto typed = samples_func_->typed<std::tuple<>, uint64_t>(store_.context());
    if (!typed) {
      return;
    }
    auto packed = typed.ok_ref().call(store_.context(), {});
    if (!packed || packed.ok() == 0) {
      return;
    }
    auto bytes = Bytes(memory_->data(store_.context()), static_cast<uint32_t>(packed.ok() >> 32),
                       static_cast<uint32_t>(packed.ok()));
    profile::StackSamples counted;
    if (bytes && counted.ParseFromArray(bytes->data(), static_cast<int>(bytes->size()))) {
      tally.Add(counted);
    }
  } else if (guest_) {
    tally.Add(guest_->Finish());
    guest_.reset();
  }
  samples_ = tally.Take();
}

profile::SlowFrame Instance::Describe() const {
  using std::chrono::duration_cast;
  using std::chrono::microseconds;
  profile::SlowFrame frame;
  frame.set_took_micros(duration_cast<microseconds>(budget_.spent).count());
  frame.set_host_micros(duration_cast<microseconds>(budget_.host).count());
  frame.set_slowest_host_call(budget_.slowest);
  frame.set_slowest_host_call_micros(duration_cast<microseconds>(budget_.slowest_time).count());
  *frame.mutable_samples() = samples_;
  return frame;
}

std::string Instance::Report() const {
  return std::format("{} took {}", budget_.call, Spent());
}

std::string Instance::Spent() const {
  // Report the time spent and the host calls' share.
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  auto text = std::format("{} ms", duration_cast<milliseconds>(budget_.spent).count());
  if (budget_.host > budget_.host.zero()) {
    text += std::format(", {} ms of it in host calls, the slowest {} at {} ms",
                        duration_cast<milliseconds>(budget_.host).count(), budget_.slowest,
                        duration_cast<milliseconds>(budget_.slowest_time).count());
  }
  return text;
}

std::string Instance::Stop(std::string trap) {
  budget_.spent = ThreadTime() - budget_.began;
  if (budget_.spent < budget_.limit) {
    return Fail(std::move(trap));
  }
  return Fail(std::format("{} ran past its {} ms budget: {}", budget_.call, budget_.limit.count(),
                          Spent()));
}

std::string Instance::Fail(std::string reason) {
  failure_ = reason;
  return reason;
}

}  // namespace modlock::wasm
