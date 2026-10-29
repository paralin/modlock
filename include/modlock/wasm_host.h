#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "modlock/plugin.h"
#include "modlock/plugin_library.h"
#include "modlock/wasm_metrics.h"
#include "modlock/wasm_settings.h"
#include "src/wasm/players/players.pb.h"
#include "src/wasm/ui/ui.pb.h"

namespace modlock {

// WasmScope is the part of a server a mod runs a game for, as when several
// mods run games for different players on one server.
struct WasmScope {
  // players are the slots the mod serves, or every player when nullopt.
  std::optional<std::set<int32_t>> players;
  // world lets a mod that serves some players still change the game for
  // every player, as the one game that owns the server's settings does. At
  // most one such mod runs at a time. A mod that serves every player always
  // may.
  bool world = false;
  // apart hides the heroes of the mod's players from every other player, and
  // every other player's hero from them.
  bool apart = false;
};

// WasmHostObserver learns what happens to a WasmHost's mods. Its methods run
// on the engine thread, or on the thread that loads the mods before the engine
// starts.
class MODLOCK_API WasmHostObserver {
 public:
  virtual ~WasmHostObserver() = default;

  // Started reports a mod that started; reloaded is true when it started
  // again, for a new build or after it stopped.
  virtual void Started(std::string_view mod, bool reloaded) = 0;

  // Logged carries one line the mod logged.
  virtual void Logged(std::string_view mod, std::string_view text) = 0;

  // Failed reports a mod that stopped, or whose restart failed. A reload that
  // does not load is reported by Reload's result instead.
  virtual void Failed(std::string_view mod, std::string_view error) = 0;

  // Ui carries a change to the interface mod shows the player in slot. A mod
  // that stops, and a player who leaves, end with a reset change.
  virtual void Ui(std::string_view mod, int32_t slot, const wasm::ui::Change& change) = 0;

  // Slowed reports an event of mod that ran past its slow time and finished,
  // with where the time went. The players may restart a mod that stays stuck.
  virtual void Slowed(std::string_view /*mod*/, std::string_view /*report*/) {}

  // Held reports that mod took or released the hold that keeps its running
  // build through reloads. The mod may still be calling the host, so an
  // observer that replaces the mod does so after the call returns.
  virtual void Held(std::string_view /*mod*/, bool /*held*/) {}
};

// WasmExtension answers mods' calls to one service the host provides. It
// receives the calling mod's name, the method and the payload, and returns
// the answer or an error the mod receives.
using WasmExtension = std::function<std::expected<std::string, std::string>(
    std::string_view mod, std::string_view method, std::string_view payload)>;

// WasmHost runs WebAssembly mods. Each mod runs in its own sandbox and reaches
// the game only through the host requests in src/wasm/wasm.proto. An
// event that runs past half a second finishes; the host warns the players in
// chat and, for five minutes, lets any of them restart the mod with
// /restartmod. A trap, a runaway event past five seconds or an exhausted
// memory budget stops that mod with a log line; the server keeps running. A
// stopped mod clears what it placed and showed and stays stopped, with a
// warning in chat each minute, until a player restarts it with /restartmod
// or a reload replaces it. Mods share one Wasmtime engine, and identical module
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
  // A mod that holds its running build keeps it, and the newest build
  // replaces it on the first frame after the mod releases the hold.
  [[nodiscard]] std::expected<void, std::string> Reload(const std::filesystem::path& path);

  // Held reports whether the running mod named mod holds its build through
  // reloads.
  [[nodiscard]] bool Held(std::string_view mod) const;

  // Scope limits the running mod named mod to scope, as when one mod runs a
  // game for some of the server's players beside another. A limited mod sees
  // only its players, hears no event or hit about another player, and is
  // refused a request that addresses another player or, unless its scope
  // holds the world, changes the whole server. A hit between one of its
  // players and a player it does not serve is blocked. Each player it stops
  // serving leaves it as a disconnected player does. A scope that asks for
  // the world while another limited mod holds it is refused.
  [[nodiscard]] std::expected<void, std::string> Scope(std::string_view mod, WasmScope scope);

  // Provide answers mods' calls to service with extension, replacing the
  // service's earlier extension. An empty extension withdraws the service.
  void Provide(std::string service, WasmExtension extension);

  // KeepSettings keeps players' settings in settings, which must stay valid
  // until the host is destroyed or keeps them elsewhere. A null settings
  // returns to the host's own, which last until it is destroyed.
  void KeepSettings(WasmSettings* settings);

  // KeepMetrics hands players' metric totals to metrics, which must stay
  // valid until the host is destroyed or hands them elsewhere. Without
  // metrics, each mod logs its players' totals.
  void KeepMetrics(WasmMetrics* metrics);

  // KeepFiles keeps the files mods write in dir, one directory per mod
  // named by its slug. Without a directory, mods read only the files their
  // builds ship and every write is refused.
  void KeepFiles(std::filesystem::path dir);

  // SettingChanged tells mod that a player changed a setting outside it,
  // such as on the player's profile, when the player is in the game. The
  // settings already hold the new value.
  void SettingChanged(std::string_view mod, uint64_t steam_id, std::string_view key,
                      std::string_view value);

  // SetRoles replaces the players' roles in the table's mod, or in every mod
  // without a table of its own when it names none. The mods read them on each
  // connection from their next event on. A table equal to the current one
  // changes nothing.
  void SetRoles(wasm::players::RolesTable roles);

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
