#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <future>
#include <map>
#include <memory>
#include <mutex>
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
// Compile is safe from any thread, so a host can compile a module away from
// the engine thread before an instance needs it.
class Runtime {
 public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Compiled is a compiled module, or why the bytes did not compile.
  using Compiled = std::expected<std::shared_ptr<const wasmtime::Module>, std::string>;

  // Compile returns the compiled module for bytes. Modules are shared by
  // content: while anyone holds the module for identical bytes, those bytes
  // are not compiled again, a call that finds the same bytes compiling on
  // another thread waits for that compile instead of starting its own, and
  // across processes Wasmtime's on-disk cache answers bytes it compiled
  // before.
  [[nodiscard]] Compiled Compile(std::span<const uint8_t> bytes);

  // Engine returns the engine every store and linker of this runtime uses.
  [[nodiscard]] wasmtime::Engine& Engine() { return engine_; }

 private:
  // Module is one entry of modules_: the compiled module while anyone holds
  // it, or the compile under way.
  struct Module {
    std::weak_ptr<const wasmtime::Module> module;
    std::shared_future<Compiled> compiling;
  };

  wasmtime::Engine engine_;
  // mu_ guards modules_, which maps module bytes to their entry; Compile
  // erases the entries that expired.
  std::mutex mu_;
  std::map<std::string, Module, std::less<>> modules_;
  // ticker_ advances the epoch; it is last so it stops first.
  std::jthread ticker_;
};

}  // namespace modlock::wasm
