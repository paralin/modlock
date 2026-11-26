#include "modlock/engine_host.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <utility>

#include "callbacks.h"
#include "modlock/gameinterop/ability_owner_guard.h"
#include "modlock/gameinterop/client_command_hook.h"
#include "modlock/gameinterop/engine_server.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/frame_hook.h"
#include "modlock/gameinterop/game_data.h"
#include "modlock/gameinterop/game_rules.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/respawn_guard.h"
#include "modlock/gameinterop/server_addons.h"
#include "modlock/gameinterop/setpawn_observe.h"
#include "modlock/gameinterop/startup_server_probe.h"
#include "modlock/gameinterop/vacated_seats.h"
#include "modlock/host_app/module_loader.h"

#if defined(_WIN32)
#include <windows.h>

#include "host_app/windows/vconsole_port.h"
#include "host_app/windows/windows_module_loader.h"
#else
#include "host_app/stub_module_loader.h"
#endif

namespace modlock {
namespace {

// ConnectionDispatch is the one sink installed at the native lifecycle hook.
class ConnectionDispatch final : public gameinterop::ConnectionEventSink {
 public:
  void OnConnected(int32_t slot, uint64_t xuid, bool bot, const char* name) override {
    connected.Dispatch(slot, xuid, bot, name);
  }

  void OnDisconnecting(int32_t slot, uint64_t xuid, int32_t reason) override {
    disconnecting.Dispatch(slot, xuid, reason);
  }

  void OnSeatEnded(int32_t slot, uint64_t xuid, int32_t reason) override {
    seat_ended.Dispatch(slot, xuid, reason);
  }

  Callbacks<void(int32_t, uint64_t, bool, const char*)> connected;
  Callbacks<void(int32_t, uint64_t, int32_t)> disconnecting;
  Callbacks<void(int32_t, uint64_t, int32_t)> seat_ended;
};

}  // namespace

struct EngineHost::Impl {
  host_app::GamePaths paths;
#if defined(_WIN32)
  host_app::WindowsModuleLoader loader;
  // Directory search registrations remain active while game modules are used.
  std::vector<DLL_DIRECTORY_COOKIE> directories;
#else
  host_app::StubModuleLoader loader;
#endif
  std::vector<std::unique_ptr<host_app::LoadedModule>> modules;
  bool opened = false;
  bool ran = false;
  // world_map names the map of the world that is ready, if any.
  std::optional<std::string> world_map;
  std::expected<int, std::string> result = std::unexpected("engine has not run");

  Callbacks<void()> frames;
  Callbacks<void(const gameinterop::NativeChatHook::Message&)> chats;
  Callbacks<bool(const gameinterop::NativePingFilter::Ping&, uint64_t&)> pings;
  Callbacks<bool(int32_t, std::string_view)> commands;
  Callbacks<void(const gameinterop::CombatEvent&)> combat;
  Callbacks<bool(const gameinterop::DamageContactEvent&)> damage;
  Callbacks<void(const gameinterop::DamageContactEvent&, float&)> adjust_damage;
  Callbacks<void(std::string_view)> before_world;
  Callbacks<void(std::string_view)> after_world;
  Callbacks<void()> ending_world;
  std::string world_error;
  Callbacks<bool(uint32_t)> respawns;
  Callbacks<void(void*, gameinterop::ConnectionTracker::SlotState)> heroes;
  // The tracker and host share its dispatch sink until the hook is restored.
  std::shared_ptr<ConnectionDispatch> connections = std::make_shared<ConnectionDispatch>();
  // vacated clears what ended seats leave. seats holds the host's own
  // subscriptions that keep seats: the seat ends, the frame that clears them
  // and releases seats away too long, and the respawn hold on their heroes.
  gameinterop::VacatedSeats vacated;
  std::optional<gameinterop::EngineServer> server;
  std::chrono::steady_clock::time_point away_checked;
  Subscription seats;
  bool holding_heroes = false;

  std::optional<gameinterop::EngineFrameHook> frame_hook;
  std::optional<gameinterop::NativeChatHook> chat_hook;
  std::optional<gameinterop::NativePingFilter> ping_filter;
  std::optional<gameinterop::ClientCommandHook> command_hook;
  std::optional<gameinterop::CombatEventsHook> combat_hook;
  std::optional<gameinterop::ConnectionTracker> connection_hook;
  std::optional<gameinterop::StartupServerProbe> world_hook;
  std::optional<gameinterop::RespawnGuard> respawn_hook;
  std::optional<gameinterop::HeroInitializationHook> hero_hook;
  std::optional<gameinterop::NativeTrace> trace;
  std::optional<gameinterop::StaminaObserver> stamina;
  std::optional<gameinterop::GameRulesHooks> rules;
  std::optional<gameinterop::SetPawnObservation> pawn_assignment;
  std::optional<gameinterop::ServerAddonsHook> addons;
  std::optional<gameinterop::AbilityOwnerGuard> ability_owner_guard;

  ~Impl() {
#if defined(_WIN32)
    for (auto cookie : directories) {
      RemoveDllDirectory(cookie);
    }
#endif
  }
};

EngineHost::EngineHost() : impl_(std::make_unique<Impl>()) {}

EngineHost::~EngineHost() = default;

bool EngineHost::Opened() const {
  return impl_->opened;
}

const host_app::GamePaths& EngineHost::Paths() const {
  return impl_->paths;
}

const std::expected<int, std::string>& EngineHost::Result() const {
  return impl_->result;
}

std::expected<void, std::string> EngineHost::Open(const std::filesystem::path& game_dir) {
  // Open once, after the game install checks out.
  if (impl_->opened || !impl_->modules.empty()) {
    return std::unexpected("game modules already opened");
  }
  impl_->paths = host_app::ResolveGamePaths(game_dir);
  if (auto problem = host_app::Validate(impl_->paths); !problem.empty()) {
    return std::unexpected(std::move(problem));
  }

#if defined(_WIN32)
  // Search the game's own directories for its modules.
  if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                LOAD_LIBRARY_SEARCH_USER_DIRS)) {
    return std::unexpected("SetDefaultDllDirectories failed: " + std::to_string(GetLastError()));
  }
  for (const auto& directory :
       {impl_->paths.engine2_dll.parent_path(), impl_->paths.server_dll.parent_path()}) {
    auto cookie = AddDllDirectory(directory.c_str());
    if (!cookie) {
      return std::unexpected("AddDllDirectory failed for " + directory.string());
    }
    impl_->directories.push_back(cookie);
  }

  // Without the shared port a second engine on this machine exits at startup;
  // a failure leaves only that second-process case unsupported.
  if (auto shared =
          host_app::ShareVConsolePort(impl_->paths.engine2_dll.parent_path() / "vconcomm.dll");
      !shared) {
    std::cerr << "[modlock] " << shared.error() << '\n';
  }

  // A failed engine assertion opens a modal dialog that holds the engine
  // thread until someone answers it, which no one does on a server. tier0
  // skips the dialog when this variable is set. Its runtime copies the
  // environment as it loads, so the variable is set before the game modules.
  if (!SetEnvironmentVariableW(L"NO_GAME_ASSERT_DIALOG", L"1")) {
    std::cerr << "[modlock] engine assertions may open dialogs: error " << GetLastError() << '\n';
  }
#endif

  // Load the game modules.
  for (const auto& module : host_app::PlanModuleLoads(impl_->paths)) {
    auto loaded = impl_->loader.Load(module.path);
    if (!loaded) {
      return std::unexpected(module.name + ": " + loaded.error());
    }
    impl_->modules.push_back(std::move(*loaded));
  }

  // Resolve the game data before any hook rewrites the code its patterns
  // read. A gap disables only the features that need it, so the host logs it
  // and plays on.
  gameinterop::ResolveGameData();

  // Keep every ability from a client its owner does not reach, whatever any
  // mod does, since such an ability crashes the client. A server without the
  // guard still runs.
  if (auto guard = gameinterop::AbilityOwnerGuard::Install(); guard) {
    impl_->ability_owner_guard.emplace(std::move(*guard));
  } else {
    std::cerr << "[modlock] ability owner guard unavailable: " << guard.error() << '\n';
  }
  impl_->opened = true;
  return {};
}

std::expected<int, std::string> EngineHost::Run(const net::LaunchConfig& launch) {
  // Run the engine once on an opened host.
  if (!impl_->opened || impl_->ran) {
    return std::unexpected("engine run requires an opened, unused host");
  }
  impl_->ran = true;
  impl_->result = net::RunEngine(launch, impl_->paths.engine2_dll.parent_path());

  // Report a world failure over the engine's result.
  if (!impl_->world_error.empty()) {
    impl_->result = std::unexpected(impl_->world_error);
  }
  return impl_->result;
}

std::expected<Subscription, std::string> EngineHost::OnFrame(std::function<void()> callback) {
  if (!impl_->frame_hook) {
    auto hook = gameinterop::EngineFrameHook::Install([this] { impl_->frames.Dispatch(); });
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->frame_hook.emplace(std::move(*hook));
  }
  return impl_->frames.Add(std::move(callback));
}

std::expected<Subscription, std::string> EngineHost::OnChat(
    gameinterop::NativeChatHook::Handler callback) {
  if (auto tracked = EnsureConnectionHook(); !tracked) {
    return std::unexpected(tracked.error());
  }
  if (!impl_->chat_hook) {
    auto hook = gameinterop::NativeChatHook::Install(
        [this](const gameinterop::NativeChatHook::Message& line) { impl_->chats.Dispatch(line); });
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->chat_hook.emplace(std::move(*hook));
  }
  return impl_->chats.Add(std::move(callback));
}

std::expected<Subscription, std::string> EngineHost::OnPing(
    gameinterop::NativePingFilter::Handler callback) {
  // The filter reads each sender's connection from the tracker.
  if (auto tracked = EnsureConnectionHook(); !tracked) {
    return std::unexpected(tracked.error());
  }
  if (!impl_->ping_filter) {
    auto filter = gameinterop::NativePingFilter::Install(
        [this](const gameinterop::NativePingFilter::Ping& ping, uint64_t& voice_muted) {
          return impl_->pings.Dispatch(ping, voice_muted);
        });
    if (!filter) {
      return std::unexpected(filter.error());
    }
    impl_->ping_filter.emplace(std::move(*filter));
  }
  return impl_->pings.Add(std::move(callback));
}

std::expected<Subscription, std::string> EngineHost::OnCommand(
    std::function<bool(int32_t, std::string_view)> callback) {
  if (auto tracked = EnsureConnectionHook(); !tracked) {
    return std::unexpected(tracked.error());
  }
  if (!impl_->command_hook) {
    auto hook =
        gameinterop::ClientCommandHook::Install([this](int32_t slot, std::string_view text) {
          return impl_->commands.Dispatch(slot, text);
        });
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->command_hook.emplace(std::move(*hook));
  }
  return impl_->commands.Add(std::move(callback));
}

std::expected<Subscription, std::string> EngineHost::OnCombat(
    gameinterop::CombatEventsHook::Handler callback,
    std::function<bool(const gameinterop::DamageContactEvent&)> suppress_damage,
    gameinterop::AdjustDamage adjust_damage) {
  // Install the combat hook on first use.
  if (!impl_->combat_hook) {
    // Install the hook once, from the server module and its schema.
    auto image = gameinterop::MappedModuleImage::ForModule(L"server.dll");
    if (!image) {
      return std::unexpected(image.error());
    }
    auto schema = gameinterop::ResolveSchemaSystem();
    if (!schema) {
      return std::unexpected(schema.error());
    }
    auto hook = gameinterop::CombatEventsHook::Install(
        *image, *schema, [this](const auto& event) { impl_->combat.Dispatch(event); },
        [this](const auto& event) { return impl_->damage.Dispatch(event); },
        [this](const auto& event, float& amount) { impl_->adjust_damage.Dispatch(event, amount); });
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->combat_hook.emplace(std::move(*hook));
  }

  // Subscribe the callback and the optional damage handlers.
  auto subscription = impl_->combat.Add(std::move(callback));
  if (suppress_damage) {
    subscription.Add(impl_->damage.Add(std::move(suppress_damage)));
  }
  if (adjust_damage) {
    subscription.Add(impl_->adjust_damage.Add(std::move(adjust_damage)));
  }
  return subscription;
}

std::expected<uint32_t, std::string> EngineHost::WatchModifierEvent(std::string_view name) {
  // The hook must be installed.
  if (!impl_->combat_hook) {
    return std::unexpected("combat events are not installed");
  }

  // Read the event's value from the running game by name, and watch it.
  auto image = gameinterop::MappedModuleImage::ForModule(L"server.dll");
  if (!image) {
    return std::unexpected(image.error());
  }
  auto id = gameinterop::ModifierEventIndex(*image, name);
  if (!id) {
    return std::unexpected(id.error());
  }
  if (auto watched = impl_->combat_hook->Watch(*id); !watched) {
    return std::unexpected(watched.error());
  }
  return *id;
}

void EngineHost::UnwatchModifierEvent(uint32_t id) {
  if (impl_->combat_hook) {
    impl_->combat_hook->Unwatch(id);
  }
}

gameinterop::ObservedInteractions EngineHost::ObservedInteractions() const {
  return impl_->combat_hook ? impl_->combat_hook->Observed() : gameinterop::ObservedInteractions{};
}

std::expected<Subscription, std::string> EngineHost::OnConnection(
    std::shared_ptr<gameinterop::ConnectionEventSink> sink) {
  // Install the connection hook for a valid sink.
  if (!sink) {
    return std::unexpected("connection subscriber is null");
  }
  if (auto tracked = EnsureConnectionHook(); !tracked) {
    return std::unexpected(tracked.error());
  }

  // Forward connects and disconnects to the sink.
  auto subscription = impl_->connections->connected.Add(
      [sink](int32_t slot, uint64_t xuid, bool bot, const char* name) {
        sink->OnConnected(slot, xuid, bot, name);
      });
  subscription.Add(
      impl_->connections->disconnecting.Add([sink](int32_t slot, uint64_t xuid, int32_t reason) {
        sink->OnDisconnecting(slot, xuid, reason);
      }));
  subscription.Add(
      impl_->connections->seat_ended.Add([sink](int32_t slot, uint64_t xuid, int32_t reason) {
        sink->OnSeatEnded(slot, xuid, reason);
      }));
  return subscription;
}

std::expected<void, std::string> EngineHost::EnsureConnectionHook() {
  if (!impl_->connection_hook) {
    auto hook = gameinterop::ConnectionTracker::Install(impl_->connections);
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->connection_hook.emplace(std::move(*hook));

    // Every host keeps seats: an ended seat's hero and controller are
    // cleared, and a seat away too long is released.
    impl_->seats.Add(impl_->connections->seat_ended.Add([this](int32_t slot, uint64_t, int32_t) {
      impl_->vacated.Vacate(slot, std::chrono::steady_clock::now());
    }));
    if (auto frames = OnFrame([this] { KeepSeats(); })) {
      impl_->seats.Add(std::move(*frames));
    } else {
      std::cerr << "[modlock] away seats keep their slots until the game restarts: "
                << frames.error() << '\n';
    }
  }
  return {};
}

void EngineHost::KeepSeats() {
  // Clear what ended seats left, and hold their heroes down once one lies.
  const auto now = std::chrono::steady_clock::now();
  impl_->vacated.Clear(now);
  if (!impl_->holding_heroes && impl_->vacated.Holding()) {
    impl_->holding_heroes = true;
    if (auto held = OnRespawn([this](uint32_t pawn) { return impl_->vacated.Blocked(pawn); })) {
      impl_->seats.Add(std::move(*held));
    } else {
      std::cerr << "[modlock] departed players' heroes may respawn: " << held.error() << '\n';
    }
  }

  // Once a second, release each seat away past the game's own limit.
  if (now - impl_->away_checked < std::chrono::seconds(1)) {
    return;
  }
  impl_->away_checked = now;
  for (const int32_t slot : gameinterop::ConnectionTracker::ExpiredAway(now)) {
    if (!impl_->server) {
      if (auto server = gameinterop::EngineServer::Resolve()) {
        impl_->server.emplace(*server);
      }
    }
    if (!impl_->server ||
        !impl_->server->DisconnectClient(slot, gameinterop::kDisconnectKicked, "away too long")) {
      gameinterop::ConnectionTracker::Release(slot, gameinterop::kDisconnectKicked);
    }
  }
}

std::expected<void, std::string> EngineHost::EnsureWorldHook() {
  if (!impl_->world_hook) {
    auto hook = gameinterop::StartupServerProbe::Install(
        [this](std::string_view map) { impl_->before_world.Dispatch(map); },
        [this](std::string_view map) {
          impl_->world_map = std::string(map);
          impl_->after_world.Dispatch(map);
        },
        [this] {
          if (!std::exchange(impl_->world_map, std::nullopt)) {
            return;
          }
          impl_->ending_world.Dispatch();
        },
        [this](std::string error) {
          impl_->world_error = std::move(error);
          auto server = gameinterop::EngineServer::Resolve();
          if (server) {
            static_cast<void>(server->ServerCommand("quit\n"));
          }
        });
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->world_hook.emplace(std::move(*hook));
  }
  return {};
}

std::expected<Subscription, std::string> EngineHost::OnWorldEnding(std::function<void()> callback) {
  if (auto ready = EnsureWorldHook(); !ready) {
    return std::unexpected(ready.error());
  }
  return impl_->ending_world.Add(std::move(callback));
}

std::expected<Subscription, std::string> EngineHost::OnWorld(
    std::function<void(std::string_view)> before, std::function<void(std::string_view)> after) {
  // Install the world hook and subscribe the before callback.
  if (auto ready = EnsureWorldHook(); !ready) {
    return std::unexpected(ready.error());
  }
  Subscription subscription;
  if (before) {
    subscription.Add(impl_->before_world.Add(std::move(before)));
  }
  if (!after) {
    return subscription;
  }

  // A subscriber that arrives while a world is ready learns of it at once.
  if (impl_->world_map) {
    after(*impl_->world_map);
  }
  subscription.Add(impl_->after_world.Add(std::move(after)));
  return subscription;
}

std::expected<Subscription, std::string> EngineHost::OnRespawn(
    std::function<bool(uint32_t)> blocked) {
  if (!impl_->respawn_hook) {
    // Install the guard once, from the server module.
    auto image = gameinterop::MappedModuleImage::ForModule(L"server.dll");
    if (!image) {
      return std::unexpected(image.error());
    }
    auto hook = gameinterop::RespawnGuard::Install(
        *image, [this](uint32_t pawn) { return impl_->respawns.Dispatch(pawn); });
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->respawn_hook.emplace(std::move(*hook));
  }
  return impl_->respawns.Add(std::move(blocked));
}

std::expected<Subscription, std::string> EngineHost::OnHeroReady(
    gameinterop::HeroReadiness& readiness) {
  if (!impl_->hero_hook) {
    // Install the hook once, from the server module.
    auto image = gameinterop::MappedModuleImage::ForModule(L"server.dll");
    if (!image) {
      return std::unexpected(image.error());
    }
    auto hook = gameinterop::HeroInitializationHook::Install(
        *image, [this](void* pawn, auto state) { impl_->heroes.Dispatch(pawn, std::move(state)); });
    if (!hook) {
      return std::unexpected(hook.error());
    }
    impl_->hero_hook.emplace(std::move(*hook));
  }
  return impl_->heroes.Add(
      [&readiness](void* pawn, auto state) { (void)readiness.Publish(pawn, std::move(state)); });
}

std::expected<gameinterop::NativeTrace*, std::string> EngineHost::Trace() {
  if (!impl_->trace) {
    // Install the trace once, from the server module.
    auto image = gameinterop::MappedModuleImage::ForModule(L"server.dll");
    if (!image) {
      return std::unexpected(image.error());
    }
    auto trace = gameinterop::NativeTrace::Install(*image);
    if (!trace) {
      return std::unexpected(trace.error());
    }
    impl_->trace.emplace(std::move(*trace));
  }
  return &*impl_->trace;
}

std::expected<void, std::string> EngineHost::ObservePawnAssignment() {
  // Install the pawn assignment observation once.
  if (impl_->pawn_assignment) {
    return {};
  }
  auto image = gameinterop::MappedModuleImage::ForModule(L"server.dll");
  if (!image) {
    return std::unexpected(image.error());
  }
  auto observation = gameinterop::SetPawnObservation::Install(*image);
  if (!observation) {
    return std::unexpected(observation.error());
  }
  impl_->pawn_assignment.emplace(std::move(*observation));
  return {};
}

std::expected<gameinterop::StaminaObserver*, std::string> EngineHost::Stamina() {
  if (!impl_->stamina) {
    auto stamina = gameinterop::StaminaObserver::Install();
    if (!stamina) {
      return std::unexpected(stamina.error());
    }
    impl_->stamina.emplace(std::move(*stamina));
  }
  return &*impl_->stamina;
}

bool EngineHost::PrecachePending() const {
  // Without the hooks nothing was asked for, so nothing waits.
  return impl_->rules && impl_->rules->PrecachePending();
}

std::expected<void, std::string> EngineHost::Precache(std::vector<std::string> heroes,
                                                      std::vector<std::string> resources) {
  // Add to the precache lists once the hooks exist.
  if (impl_->rules) {
    impl_->rules->AddPrecache(std::move(heroes), std::move(resources));
    return {};
  }

  // Otherwise install the game rules hooks with these lists.
  auto image = gameinterop::MappedModuleImage::ForModule(L"server.dll");
  if (!image) {
    return std::unexpected(image.error());
  }
  auto rules =
      gameinterop::GameRulesHooks::Install(*image, std::move(heroes), std::move(resources));
  if (!rules) {
    return std::unexpected(rules.error());
  }
  impl_->rules.emplace(std::move(*rules));
  return {};
}

std::expected<void, std::string> EngineHost::AdvertiseAddons(std::string addons) {
  // Advertise the addons once.
  if (impl_->addons) {
    return std::unexpected("content addons already advertised");
  }
  auto hook = gameinterop::ServerAddonsHook::Install(std::move(addons));
  if (!hook) {
    return std::unexpected(hook.error());
  }
  impl_->addons.emplace(std::move(*hook));
  return {};
}

}  // namespace modlock
