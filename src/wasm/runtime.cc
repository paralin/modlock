//go:build deps_only

#include "wasm/runtime.h"

#include <condition_variable>
#include <future>
#include <mutex>
#include <string_view>
#include <utility>

namespace modlock::wasm {
namespace {

// EpochEngine returns an engine whose stores can carry an epoch deadline and
// whose modules may throw, as the Luau runtime does. Its compiled modules
// stay in Wasmtime's cache directory, keyed by the module, the engine's
// settings and Wasmtime's version, so a restarted server loads each
// interpreter and mod without compiling it again. Without a usable cache
// directory every start compiles, as before.
wasmtime::Engine EpochEngine() {
  // Interrupt by epoch and cache compiled modules.
  wasmtime::Config config;
  config.epoch_interruption(true);
  config.wasm_exceptions(true);
  static_cast<void>(config.cache_load_default());
  return wasmtime::Engine(std::move(config));
}

}  // namespace

Runtime::Runtime() : engine_(EpochEngine()) {
  // Advance the epoch so a running call meets its deadline.
  ticker_ = std::jthread([engine = engine_](std::stop_token stop) mutable {
    // Tick the epoch until the host stops.
    std::mutex mu;
    std::condition_variable_any wake;
    std::unique_lock lock(mu);
    auto stopped = [&stop] {
      return stop.stop_requested();
    };
    while (!wake.wait_for(lock, stop, kEpochTick, stopped)) {
      engine.increment_epoch();
    }
  });
}

Runtime::~Runtime() = default;

Runtime::Compiled Runtime::Compile(std::span<const uint8_t> bytes) {
  // Reuse the module someone holds, join the compile under way, or start one.
  const std::string_view key(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  std::promise<Compiled> promise;
  std::shared_future<Compiled> compiling;
  {
    std::lock_guard lock(mu_);
    if (auto found = modules_.find(key); found != modules_.end()) {
      if (auto module = found->second.module.lock()) {
        return module;
      }
      compiling = found->second.compiling;
    }
    if (!compiling.valid()) {
      std::erase_if(modules_, [](const auto& entry) {
        return !entry.second.compiling.valid() && entry.second.module.expired();
      });
      modules_.insert_or_assign(std::string(key),
                                Module{.compiling = promise.get_future().share()});
    }
  }
  if (compiling.valid()) {
    return compiling.get();
  }

  // Compile the bytes outside the lock; Wasmtime only reads them.
  auto result = wasmtime::Module::compile(
      engine_, wasmtime::Span<uint8_t>(const_cast<uint8_t*>(bytes.data()), bytes.size()));
  Compiled compiled =
      result ? Compiled(std::make_shared<const wasmtime::Module>(std::move(result.ok_ref())))
             : std::unexpected("cannot compile the mod: " + result.err().message());

  // Publish the module, or forget the bytes so a later call tries again, and
  // wake the callers that joined.
  {
    std::lock_guard lock(mu_);
    if (auto found = modules_.find(key); found != modules_.end()) {
      if (compiled) {
        found->second = Module{.module = *compiled};
      } else {
        modules_.erase(found);
      }
    }
  }
  promise.set_value(compiled);
  return compiled;
}

}  // namespace modlock::wasm
