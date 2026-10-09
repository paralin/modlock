#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <wasmtime.hh>

#include "proto/modlock/wasm.pb.h"
#include "wasm/runtime.h"

namespace modlock::wasm {

// Limits bounds what one mod may consume. A mod that exceeds a limit traps,
// and only that mod stops.
struct Limits {
  // memory_bytes caps each linear memory the mod creates or grows.
  int64_t memory_bytes = int64_t{256} << 20;
  // start_budget bounds module initialization, which runs package setup,
  // and the Start event, where a script mod evaluates its whole script. Both
  // run while the server loads the mod, before players are in it.
  std::chrono::milliseconds start_budget{5000};
  // event_budget bounds one event, including the host calls it makes. The
  // engine drops a player whose commands hold its thread for about a second,
  // so the budget is half that, which still fits one slow engine call such as
  // adding a bot (about 270 ms). Budgets count the engine thread's processor
  // time, the interpreter's garbage collection included: a stall in which
  // the thread waits, such as preemption or paging on a machine short of
  // memory, spends none of it, since stopping the mod would not end it.
  std::chrono::milliseconds event_budget{500};
};

// Instance runs one WebAssembly mod in its own Wasmtime store. The mod sees
// WASI preview 1 with standard output and error but no files, environment,
// arguments or network, and the two modlock imports: host_call, which hands
// the host a Call, and host_read, which copies the host's pending Call or
// Reply into mod memory. Deliver calls the mod's modlock_event export. A trap,
// an exhausted budget or a boundary violation fails the instance permanently;
// later deliveries return that failure.
//
// All methods run on one thread at a time, normally the engine thread. The
// runtime's epoch ticker is the only other thread.
class Instance {
 public:
  // HostCall answers one call from the mod. It runs inside Deliver or Load on
  // the calling thread.
  using HostCall = std::function<Reply(const Call&)>;

  ~Instance();
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;

  // Load compiles module in runtime, or reuses its compiled copy,
  // instantiates it and runs its _initialize export within
  // limits.start_budget. host_call answers the mod's calls for the instance
  // lifetime, starting during initialization. runtime must outlive the
  // instance.
  [[nodiscard]] static std::expected<std::unique_ptr<Instance>, std::string> Load(
      Runtime& runtime, std::span<const uint8_t> module, const Limits& limits, HostCall host_call);

  // Deliver hands call to the mod and returns its reply, within
  // limits.start_budget for the Start event and limits.event_budget for any
  // other. A nested delivery from inside a host call is refused without
  // failing the instance.
  [[nodiscard]] std::expected<Reply, std::string> Deliver(const Call& call);

  // Failure returns the reason the instance stopped, or nullopt while it runs.
  [[nodiscard]] const std::optional<std::string>& Failure() const { return failure_; }

 private:
  Instance(Runtime& runtime, const Limits& limits, HostCall host_call);

  std::expected<void, std::string> Instantiate(std::span<const uint8_t> module);
  std::expected<void, std::string> DefineImports(wasmtime::Linker& linker);
  wasmtime::Result<uint32_t, wasmtime::Trap> HostCallImport(wasmtime::Caller caller, uint32_t data,
                                                            uint32_t size);
  wasmtime::Result<std::monostate, wasmtime::Trap> HostReadImport(wasmtime::Caller caller,
                                                                  uint32_t data, uint32_t size);
  // Budget is the running call's processor time limit and how its host calls
  // spent it. began is the thread's processor time when the call started.
  struct Budget {
    std::string call;
    std::chrono::milliseconds limit{};
    std::chrono::nanoseconds began{};
    std::chrono::nanoseconds host{};
    std::string slowest;
    std::chrono::nanoseconds slowest_time{};
  };

  // SetBudget starts the budget limit for call.
  void SetBudget(std::string call, std::chrono::milliseconds limit);
  // Left returns the processor time left in budget_, below zero once spent.
  std::chrono::nanoseconds Left() const;
  // Ticks returns the epoch ticks that cover left, zero once it is spent.
  static uint64_t Ticks(std::chrono::nanoseconds left);
  // ArmDeadline sets the epoch deadline to the time left in budget_.
  void ArmDeadline();
  // Stop fails the instance after a trap: an overrun budget, which it
  // describes with where the time went, or else the trap itself.
  std::string Stop(std::string trap);
  std::string Fail(std::string reason);

  // runtime_ compiles the module and advances the epoch the budgets count.
  Runtime& runtime_;
  // module_ keeps the compiled module shared while this instance lives.
  std::shared_ptr<const wasmtime::Module> module_;
  Limits limits_;
  HostCall host_call_;
  // store_ owns the instance, its memory and its WASI state.
  wasmtime::Store store_;
  std::optional<wasmtime::Memory> memory_;
  std::optional<wasmtime::Func> event_;
  // pending_ holds the encoded message host_read copies next.
  std::string pending_;
  Budget budget_;
  // busy_ is true while a call into the mod is running.
  bool busy_ = false;
  std::optional<std::string> failure_;
};

}  // namespace modlock::wasm
