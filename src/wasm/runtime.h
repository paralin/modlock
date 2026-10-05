#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <wasmtime.hh>

namespace modlock::wasm {

// kEpochTick is how often the runtime advances the engine epoch; instance
// budgets are rounded up to whole ticks.
inline constexpr std::chrono::milliseconds kEpochTick{10};

// Runtime holds what every mod shares: one Wasmtime engine with epoch
// interruption, the ticker thread that advances its epoch, and the compiled
// modules that live instances use. It must outlive every Instance it loads.
// Compile runs on one thread at a time, normally the engine thread.
class Runtime {
 public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Compile returns the compiled module for bytes. Modules are shared by
  // content: while any instance holds the module for identical bytes, those
  // bytes are not compiled again.
  [[nodiscard]] std::expected<std::shared_ptr<const wasmtime::Module>, std::string> Compile(
      std::span<const uint8_t> bytes);

  // Engine returns the engine every store and linker of this runtime uses.
  [[nodiscard]] wasmtime::Engine& Engine() { return engine_; }

 private:
  wasmtime::Engine engine_;
  // modules_ maps module bytes to the compiled module while an instance holds
  // it; Compile erases the entries that expired.
  std::map<std::string, std::weak_ptr<const wasmtime::Module>, std::less<>> modules_;
  // ticker_ advances the epoch; it is last so it stops first.
  std::jthread ticker_;
};

}  // namespace modlock::wasm
