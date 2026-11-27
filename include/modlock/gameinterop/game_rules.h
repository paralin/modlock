#pragma once

#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// KothState contains observed native fields, not an inferred round winner.
struct KothState {
  int32_t scoring_team;
  float scoring_time;
  float cash_in_started;
  float give_up_time;
  float next_spawn;
  float spawn_window;
};

// KothRules borrows the mapped server module for its lifetime. Resolve after
// schema initialization; Read only on the engine thread while its world is ready.
// Each operation reloads the current rules pointer; no world object is retained.
class MODLOCK_API KothRules {
 public:
  static std::expected<KothRules, std::string> Resolve(const ModuleImage& server,
                                                       void* schema_system);
  std::expected<KothState, std::string> Read() const;
  // StartAt supplies an authored location to the normal native warning/spawn
  // routine. It does not teleport players or invent an objective lifetime.
  std::expected<void, std::string> StartAt(const std::array<float, 3>& position);
  // Reset clears native scoring and spawn timers after the active objective
  // actors have been removed. Autonomous KOTH spawning must remain disabled.
  std::expected<void, std::string> Reset();

 private:
  std::expected<void*, std::string> Current() const;
  void* const* current_ = nullptr;
  void (*start_)(void*) = nullptr;
  size_t next_position_offset_ = 0;
  std::optional<float> started_after_scoring_;
  std::array<size_t, 6> offsets_{};
  uintptr_t module_begin_ = 0;
  size_t module_size_ = 0;
};

// ResolveCurrentGameRules locates the engine-owned pointer through the native
// force-KOTH callback, rejecting ambiguous signatures and out-of-module targets.
MODLOCK_API std::expected<void* const*, std::string> ResolveCurrentGameRules(
    const ModuleImage& server);

// GamePause uses the native game-rules pause operation, including its pause
// modifier and replication. Calls run on the engine thread; the mapped server
// module owns the current rules and callback for this object's lifetime.
class MODLOCK_API GamePause {
 public:
  static std::expected<GamePause, std::string> Resolve(const ModuleImage& server,
                                                       void* schema_system);
  // Set also refreshes pause modifiers on newly created entities when already paused.
  std::expected<void, std::string> Set(bool paused) const;
  // Paused reports whether the rules hold the server's pause, the one Set
  // changes. With no active rules the game is not paused.
  std::expected<bool, std::string> Paused() const;

 private:
  void* const* current_ = nullptr;
  void (*toggle_)() = nullptr;
  void (*refresh_)(void*) = nullptr;
  size_t paused_offset_ = 0;
};

// PrecacheList holds the hero and resource names every session manifest
// build registers, each once, in the order first added. Names stay for the
// life of the list, so a world that registered a name keeps it in every later
// manifest. The list also tracks whether a name arrived after the last build:
// such a name reaches the game only with the next map load. It is not
// synchronized; the game thread both adds and builds.
class MODLOCK_API PrecacheList {
 public:
  // Add appends each name the list lacks and reports whether it added any.
  bool Add(std::vector<std::string> heroes, std::vector<std::string> resources);

  // Built records that a manifest build registered every name held.
  void Built() { pending_ = false; }

  // Pending reports whether Add appended a name since the last Built, so the
  // loaded world lacks it until the map loads again.
  [[nodiscard]] bool Pending() const { return pending_; }

  [[nodiscard]] const std::vector<std::string>& heroes() const { return heroes_; }

  [[nodiscard]] const std::vector<std::string>& resources() const { return resources_; }

 private:
  std::vector<std::string> heroes_;
  std::vector<std::string> resources_;
  bool pending_ = false;
};

// GameRulesHooks interposes CCitadelGameRules::BuildGameSessionManifest
// through a native detour. The engine builds one session
// manifest per map activation; the hook lets the original build it, then
// precaches the configured heroes into the manifest through the game's own
// precache routine, decoded from the function's own bytes:
//
//   global  = REX.W LEA at +0x389 from the manifest builder's start
//   precache= E8 call    at +0x394
//
// Without this, a dedicated server builds a manifest with zero heroes and a
// connecting client crashes rendering the empty world. One instance may
// exist at a time; destruction disables the hook.
class MODLOCK_API GameRulesHooks {
 public:
  using AddResourceFn = void (*)(const char* path, void* manifest);

  // RegisterResources invokes the native resource function in caller order.
  // The manifest is borrowed only during its synchronous build callback.
  static bool RegisterResources(void* manifest, AddResourceFn add_resource,
                                std::span<const std::string> resources);

  // Install scans server.dll for the manifest builder, decodes the precache
  // pair from its own instruction stream, and hooks it. Failure names the
  // rejected shape (module image, signature, or decode).
  static std::expected<GameRulesHooks, std::string> Install(
      const ModuleImage& server, std::vector<std::string> precache_heroes,
      std::vector<std::string> resources = {});

  // AddPrecache adds plugin-supplied resources before the next manifest build.
  void AddPrecache(std::vector<std::string> heroes, std::vector<std::string> resources);
  // PrecachePending reports whether AddPrecache named a hero or resource that
  // no manifest build has registered yet; the next map load registers it.
  [[nodiscard]] bool PrecachePending() const;

  GameRulesHooks(GameRulesHooks&&) noexcept;
  GameRulesHooks& operator=(GameRulesHooks&&) noexcept;
  ~GameRulesHooks();

 private:
  struct Impl;
  explicit GameRulesHooks(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
