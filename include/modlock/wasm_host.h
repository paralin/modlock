#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "modlock/plugin.h"
#include "modlock/plugin_library.h"
#include "proto/modlock/ui.pb.h"

namespace modlock {

// WasmHostObserver learns what happens to a WasmHost's mods. Its methods run
// on the engine thread, or on the thread that loads the mods before the engine
// starts.
class MODLOCK_API WasmHostObserver {
 public:
  virtual ~WasmHostObserver() = default;

  // Started reports a mod that started; reloaded is true when a new build
  // replaced a running one.
  virtual void Started(std::string_view mod, bool reloaded) = 0;

  // Logged carries one line the mod logged.
  virtual void Logged(std::string_view mod, std::string_view text) = 0;

  // Failed reports a mod that stopped. A reload that does not load is
  // reported by Reload's result instead.
  virtual void Failed(std::string_view mod, std::string_view error) = 0;

  // Ui carries a change to the interface mod shows the player in slot. A mod
  // that stops, and a player who leaves, end with a reset change.
  virtual void Ui(std::string_view mod, int32_t slot, const ui::Change& change) = 0;
};

// WasmExtension answers mods' calls to one service the host provides. It
// receives the calling mod's name, the method and the payload, and returns
// the answer or an error the mod receives.
using WasmExtension = std::function<std::expected<std::string, std::string>(
    std::string_view mod, std::string_view method, std::string_view payload)>;

// WasmHost runs WebAssembly mods. Each mod runs in its own sandbox and reaches
// the game only through the host requests in proto/modlock/wasm.proto. A trap
// or an exhausted time or memory budget stops that mod with a log line; the
// server keeps running. Mods share one Wasmtime engine, and identical module
// bytes compile once while a mod uses them, so the mods of one interpreted
// language share one compiled interpreter.
//
// The host must outlive every plugin it loads. Its methods run on the engine
// thread, or before the engine starts.
class MODLOCK_API WasmHost {
 public:
  // interpreters is the directory with the interpreter modules that run
  // interpreted mods, such as quickjs.wasm for JavaScript.
  explicit WasmHost(std::filesystem::path interpreters);
  ~WasmHost();
  WasmHost(const WasmHost&) = delete;
  WasmHost& operator=(const WasmHost&) = delete;

  // Observe reports what happens to the mods to observer as well as to the
  // host's other observers. observer must stay valid until Unobserve.
  void Observe(WasmHostObserver* observer);

  // Unobserve stops reporting to observer.
  void Unobserve(WasmHostObserver* observer);

  // Load reads a built mod, either a directory with mod.json or a .wasm file,
  // runs its initialization, and returns a plugin that delivers server frames
  // and player commands to it. A directory's mod is named by its manifest's
  // slug, a file's by its stem.
  [[nodiscard]] std::expected<std::unique_ptr<Plugin>, std::string> Load(
      const std::filesystem::path& path, const PluginContext& context);

  // Reload replaces the running mod of the same name with the build at path.
  // The mod keeps its place among the plugins: its old build stops and the
  // new one starts. A build that does not load leaves the old one running.
  [[nodiscard]] std::expected<void, std::string> Reload(const std::filesystem::path& path);

  // Provide answers mods' calls to service with extension, replacing the
  // service's earlier extension. An empty extension withdraws the service.
  void Provide(std::string service, WasmExtension extension);

  // Call calls method of the service mod serves and returns the mod's answer.
  // A mod that is not running, serves no such service or fails returns an
  // error. A mod cannot be called while it is calling the host.
  [[nodiscard]] std::expected<std::string, std::string> Call(std::string_view mod,
                                                             std::string_view service,
                                                             std::string_view method,
                                                             std::string_view payload);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock
