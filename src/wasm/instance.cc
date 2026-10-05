#include "wasm/instance.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>
#include <variant>

namespace modlock::wasm {
namespace {

// Bytes returns the mod memory range [data, data + size), or nullopt when the
// range leaves memory.
std::optional<std::span<uint8_t>> Bytes(wasmtime::Span<uint8_t> memory, uint32_t data,
                                        uint32_t size) {
  if (uint64_t{data} + size > memory.size()) return std::nullopt;
  return std::span<uint8_t>(memory.data() + data, size);
}

// CallerBytes resolves a range of the calling mod's exported memory.
std::optional<std::span<uint8_t>> CallerBytes(wasmtime::Caller& caller, uint32_t data,
                                              uint32_t size) {
  auto exported = caller.get_export("memory");
  if (!exported) return std::nullopt;
  auto* memory = std::get_if<wasmtime::Memory>(&*exported);
  if (!memory) return std::nullopt;
  return Bytes(memory->data(caller.context()), data, size);
}

}  // namespace

Instance::Instance(Runtime& runtime, const Limits& limits, HostCall host_call)
    : runtime_(runtime),
      limits_(limits),
      host_call_(std::move(host_call)),
      store_(runtime.Engine()) {
  store_.limiter(limits_.memory_bytes, -1, -1, -1, -1);
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
  if (!compiled) return std::unexpected(compiled.error());
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
  if (auto defined = DefineImports(linker); !defined) return defined;
  SetBudget(limits_.start_budget);
  auto instance = linker.instantiate(store_.context(), *module_);
  if (!instance) return std::unexpected("cannot instantiate the mod: " + instance.err().message());

  // Find the memory and the event entry point the boundary needs.
  auto context = store_.context();
  auto memory = instance.ok_ref().get(context, "memory");
  if (memory) {
    if (auto* exported = std::get_if<wasmtime::Memory>(&*memory)) memory_ = *exported;
  }
  auto event = instance.ok_ref().get(context, "modlock_event");
  if (event) {
    if (auto* exported = std::get_if<wasmtime::Func>(&*event)) event_ = *exported;
  }
  if (!memory_ || !event_) {
    return std::unexpected("the mod must export memory and modlock_event");
  }

  // Run the reactor's initialization, which registers the mod's handlers.
  auto initialize = instance.ok_ref().get(context, "_initialize");
  auto* function = initialize ? std::get_if<wasmtime::Func>(&*initialize) : nullptr;
  if (!function) return {};
  busy_ = true;
  auto initialized = function->call(context, {});
  busy_ = false;
  if (!initialized) return std::unexpected(Fail(initialized.err().message()));
  return {};
}

std::expected<void, std::string> Instance::DefineImports(wasmtime::Linker& linker) {
  auto call = linker.func_wrap("modlock", "host_call",
                               [this](wasmtime::Caller caller, uint32_t data, uint32_t size) {
                                 return HostCallImport(caller, data, size);
                               });
  if (!call) return std::unexpected("cannot define host_call: " + call.err().message());
  auto read = linker.func_wrap("modlock", "host_read",
                               [this](wasmtime::Caller caller, uint32_t data, uint32_t size) {
                                 return HostReadImport(caller, data, size);
                               });
  if (!read) return std::unexpected("cannot define host_read: " + read.err().message());
  return {};
}

std::expected<Reply, std::string> Instance::Deliver(const Call& call) {
  if (failure_) return std::unexpected(*failure_);
  if (busy_) return std::unexpected("the mod is already handling an event");

  // Call the mod, which copies the call out with host_read.
  auto typed = event_->typed<uint32_t, uint64_t>(store_.context());
  if (!typed) return std::unexpected(Fail("modlock_event must take i32 and return i64"));
  call.SerializeToString(&pending_);
  SetBudget(limits_.event_budget);
  busy_ = true;
  auto called = typed.ok_ref().call(store_.context(), static_cast<uint32_t>(pending_.size()));
  busy_ = false;
  pending_.clear();
  if (!called) return std::unexpected(Fail(called.err().message()));

  // Decode the reply the mod left in its memory.
  const uint64_t packed = called.ok();
  Reply reply;
  if (packed == 0) return reply;
  auto bytes = Bytes(memory_->data(store_.context()), static_cast<uint32_t>(packed >> 32),
                     static_cast<uint32_t>(packed));
  if (!bytes) return std::unexpected(Fail("modlock_event returned a reply outside memory"));
  if (!reply.ParseFromArray(bytes->data(), static_cast<int>(bytes->size()))) {
    return std::unexpected(Fail("modlock_event returned an invalid Reply"));
  }
  return reply;
}

wasmtime::Result<uint32_t, wasmtime::Trap> Instance::HostCallImport(wasmtime::Caller caller,
                                                                    uint32_t data, uint32_t size) {
  // Decode the call from mod memory.
  auto bytes = CallerBytes(caller, data, size);
  if (!bytes) return wasmtime::Trap("modlock.host_call: the call is outside memory");
  Call call;
  if (!call.ParseFromArray(bytes->data(), static_cast<int>(bytes->size()))) {
    return wasmtime::Trap("modlock.host_call: invalid Call");
  }

  // Answer it and hold the encoded reply for host_read.
  const auto entered = std::chrono::steady_clock::now();
  host_call_(call).SerializeToString(&pending_);
  deadline_ += std::chrono::steady_clock::now() - entered;
  ArmDeadline();
  return static_cast<uint32_t>(pending_.size());
}

wasmtime::Result<std::monostate, wasmtime::Trap> Instance::HostReadImport(wasmtime::Caller caller,
                                                                          uint32_t data,
                                                                          uint32_t size) {
  if (size != pending_.size()) {
    return wasmtime::Trap("modlock.host_read: the size differs from the pending message");
  }
  auto bytes = CallerBytes(caller, data, size);
  if (!bytes) return wasmtime::Trap("modlock.host_read: the buffer is outside memory");
  std::memcpy(bytes->data(), pending_.data(), size);
  pending_.clear();
  return std::monostate{};
}

void Instance::SetBudget(std::chrono::milliseconds budget) {
  deadline_ = std::chrono::steady_clock::now() + budget;
  ArmDeadline();
}

void Instance::ArmDeadline() {
  const auto left = std::max(std::chrono::steady_clock::duration::zero(),
                             deadline_ - std::chrono::steady_clock::now());
  store_.context().set_epoch_deadline((left + kEpochTick - std::chrono::nanoseconds{1}) /
                                      kEpochTick);
}

std::string Instance::Fail(std::string reason) {
  failure_ = reason;
  return reason;
}

}  // namespace modlock::wasm
