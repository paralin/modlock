#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/combat_events.h"
#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/hero_initialization.h"
#include "modlock/gameinterop/native_chat_hook.h"
#include "modlock/gameinterop/native_trace.h"
#include "modlock/gameinterop/stamina_observer.h"
#include "modlock/host_app/game_paths.h"
#include "modlock/net/listen_boot.h"
#include "modlock/subscription.h"

namespace modlock {

// EngineHost owns game modules and each native hook exactly once. Plugins borrow
// its engine capabilities and own subscriptions; they never own the engine loop.
// The host outlives every plugin and subscription. Engine operations and callbacks
// run on the engine thread; Stop and destruction follow the engine's return.
class MODLOCK_API EngineHost {
 public:
  EngineHost();
  ~EngineHost();
  EngineHost(const EngineHost&) = delete;
  EngineHost& operator=(const EngineHost&) = delete;

  // Open validates a game installation and loads the modules its launch role
  // requires, once. A client launch also maps client.dll so plugins can hook
  // it before Run starts the engine.
  std::expected<void, std::string> Open(const std::filesystem::path& game_dir,
                                        const net::LaunchConfig& launch);
  const host_app::GamePaths& Paths() const;

  // Run hands control to the engine until it exits, then invalidates world views.
  std::expected<int, std::string> Run(const net::LaunchConfig& launch);
  const std::expected<int, std::string>& Result() const;

  // Event subscriptions dispatch in registration order. A true command,
  // damage, or respawn decision claims that operation and ends its dispatch.
  std::expected<Subscription, std::string> OnFrame(std::function<void()> callback);
  std::expected<Subscription, std::string> OnChat(gameinterop::NativeChatHook::Handler callback);
  std::expected<Subscription, std::string> OnCommand(
      std::function<bool(int32_t, std::string_view)> callback);
  std::expected<Subscription, std::string> OnCombat(
      gameinterop::CombatEventsHook::Handler callback,
      std::function<bool(const gameinterop::DamageContactEvent&)> suppress_damage = {},
      gameinterop::AdjustDamage adjust_damage = {});
  std::expected<Subscription, std::string> OnConnection(
      std::shared_ptr<gameinterop::ConnectionEventSink> sink);
  // OnWorld runs before as each world's map starts loading and after once it
  // is ready; a world already ready runs after during the call.
  std::expected<Subscription, std::string> OnWorld(std::function<void(std::string_view)> before,
                                                   std::function<void(std::string_view)> after);
  std::expected<Subscription, std::string> OnRespawn(std::function<bool(uint32_t)> blocked);
  // OnWorldEnding runs before INetworkGameServer::Shutdown destroys the world.
  // Remove plugin entities here; Stop releases subscriptions after engine return.
  std::expected<Subscription, std::string> OnWorldEnding(std::function<void()> callback);
  // OnHeroReady gives each consumer its own one-use readiness queue.
  std::expected<Subscription, std::string> OnHeroReady(gameinterop::HeroReadiness& readiness);
  // ObservePawnAssignment enables the optional engine assignment diagnostic once.
  std::expected<void, std::string> ObservePawnAssignment();

  // Trace and Stamina share their installed native hooks across consumers.
  // Returned views remain valid until host destruction and carry their native
  // world-invalidation contracts.
  std::expected<gameinterop::NativeTrace*, std::string> Trace();
  std::expected<gameinterop::StaminaObserver*, std::string> Stamina();

  // Precache contributes engine resource names to the next session manifest.
  // Resource selection belongs to the plugin; the framework deduplicates them.
  std::expected<void, std::string> Precache(std::vector<std::string> heroes,
                                            std::vector<std::string> resources);

  // AdvertiseAddons offers comma-separated content addon names to every
  // connecting client, once per host, before Run starts the listen server.
  std::expected<void, std::string> AdvertiseAddons(std::string addons);

 private:
  std::expected<void, std::string> EnsureWorldHook();
  // EnsureConnectionHook installs the tracker that chat and command hooks
  // consult to admit only connected players.
  std::expected<void, std::string> EnsureConnectionHook();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock
