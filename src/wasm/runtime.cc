#include "wasm/runtime.h"

#include <condition_variable>
#include <mutex>
#include <string_view>
#include <utility>

namespace modlock::wasm {
namespace {

// EpochEngine returns an engine whose stores can carry an epoch deadline and
// whose modules may throw, as the Luau runtime does.
wasmtime::Engine EpochEngine() {
  wasmtime::Config config;
  config.epoch_interruption(true);
  config.wasm_exceptions(true);
  return wasmtime::Engine(std::move(config));
}

}  // namespace

Runtime::Runtime() : engine_(EpochEngine()) {
  // Advance the epoch so a running call meets its deadline.
  ticker_ = std::jthread([engine = engine_](std::stop_token stop) mutable {
    std::mutex mu;
    std::condition_variable_any wake;
    std::unique_lock lock(mu);
    auto stopped = [&stop] { return stop.stop_requested(); };
    while (!wake.wait_for(lock, stop, kEpochTick, stopped)) engine.increment_epoch();
  });
}

Runtime::~Runtime() = default;

std::expected<std::shared_ptr<const wasmtime::Module>, std::string> Runtime::Compile(
    std::span<const uint8_t> bytes) {
  // Reuse the module a live instance already holds.
  const std::string_view key(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (auto found = modules_.find(key); found != modules_.end()) {
    if (auto module = found->second.lock()) return module;
  }
  std::erase_if(modules_, [](const auto& entry) { return entry.second.expired(); });

  // Compile the bytes; Wasmtime only reads them.
  auto compiled = wasmtime::Module::compile(
      engine_, wasmtime::Span<uint8_t>(const_cast<uint8_t*>(bytes.data()), bytes.size()));
  if (!compiled) return std::unexpected("cannot compile the mod: " + compiled.err().message());
  auto module = std::make_shared<const wasmtime::Module>(std::move(compiled.ok_ref()));
  modules_.insert_or_assign(std::string(key), module);
  return module;
}

}  // namespace modlock::wasm
