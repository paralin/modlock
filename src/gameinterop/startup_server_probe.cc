#include "modlock/gameinterop/startup_server_probe.h"

#include <optional>
#include <utility>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/virtual_slots.h"

#if defined(_WIN32)
#include <windows.h>

#include <safetyhook.hpp>
#endif

namespace modlock::gameinterop {
namespace {

// The thunk state: one probe exists at a time and shares nothing with the
// other hooks' globals.
void* g_startup_server_original = nullptr;
StartupServerProbe::StartupCallback g_before_startup;
StartupServerProbe::StartupCallback g_after_startup;
std::function<void()> g_before_shutdown;
std::function<void(std::string)> g_on_error;
#if defined(_WIN32)
std::optional<safetyhook::InlineHook> g_shutdown_hook;
#endif

// ClearThunkState drops the singleton thunk inputs. Callers restore their
// dispatch-table entry first: while an entry still points at the thunk, a null
// original would swallow every engine session startup.
void ClearThunkState() {
#if defined(_WIN32)
  // Unhook shutdown and drop every callback.
  g_shutdown_hook.reset();
#endif
  g_before_shutdown = {};
  g_on_error = {};
  g_startup_server_original = nullptr;
  g_before_startup = nullptr;
  g_after_startup = nullptr;
}

#if defined(_WIN32)
using StartupServerFn = void (*)(void* self, const void* config, void* world_session,
                                 const char* map_name);

// INetworkGameServer::Shutdown's dispatch table belongs to engine2.dll and
// outlives the server instance.
void ShutdownServerThunk(void* self) {
  if (g_before_shutdown) {
    g_before_shutdown();
  }
  g_shutdown_hook->call<void>(self);
}

std::expected<void, std::string> ArmShutdown(void* service) {
  // Resolve the server service's slots.
  const auto game_server_slot = ResolveVirtualSlot("network-server-service.game-server");
  const auto shutdown_slot = ResolveVirtualSlot("network-game-server.shutdown");
  if (!game_server_slot) {
    return std::unexpected(game_server_slot.error());
  }
  if (!shutdown_slot) {
    return std::unexpected(shutdown_slot.error());
  }

  // Find the network game server.
  auto table = *static_cast<void***>(service);
  auto server = reinterpret_cast<void* (*)(void*)>(table[*game_server_slot])(service);
  if (!server) {
    return std::unexpected("network game server unavailable after startup");
  }
  g_shutdown_hook.reset();
  auto server_table = *static_cast<void***>(server);

  // The engine calls this implementation directly during normal quit, so an
  // inline hook is required; replacing only the virtual slot misses teardown.
  auto hook = safetyhook::create_inline(server_table[*shutdown_slot],
                                        reinterpret_cast<void*>(&ShutdownServerThunk));
  if (!hook) {
    return std::unexpected("could not hook network game server shutdown");
  }
  g_shutdown_hook.emplace(std::move(hook));
  return {};
}

// StartupServerThunk runs the before callback immediately before the engine
// original, then runs the after callback while the session is pre-admission.
void StartupServerThunk(void* self, const void* config, void* world_session, const char* map_name) {
  StartupServerProbe::DispatchCallbacks(
      map_name, g_before_startup,
      [&](std::string_view /*borrowed_map_name*/) {
        if (g_startup_server_original != nullptr) {
          reinterpret_cast<StartupServerFn>(g_startup_server_original)(self, config, world_session,
                                                                       map_name);
        }
      },
      [&](std::string_view map) {
        if (auto armed = ArmShutdown(self); !armed) {
          if (g_on_error) {
            g_on_error(armed.error());
          }
          return;
        }
        if (g_after_startup) {
          g_after_startup(map);
        }
      });
}
#endif

}  // namespace

StartupServerProbe::StartupServerProbe(ThunkOwner owner) : owner_(std::move(owner)) {}

void StartupServerProbe::DispatchCallbacks(std::string_view map_name,
                                           const StartupCallback& before_startup,
                                           const StartupCallback& original,
                                           const StartupCallback& after_startup) {
  if (before_startup) {
    before_startup(map_name);
  }
  if (original) {
    original(map_name);
  }
  if (after_startup) {
    after_startup(map_name);
  }
}

std::expected<StartupServerProbe, std::string> StartupServerProbe::Install(
    StartupCallback before_startup, StartupCallback after_startup,
    std::function<void()> before_shutdown, std::function<void(std::string)> on_error) {
#if defined(_WIN32)
  // Find the network server service's startup slot.
  const auto resolved = ResolveEngineInterface(L"engine2.dll", kNetworkServerServiceVersion);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  void* service = *resolved;
  const auto slot = ResolveVirtualSlot("network-server-service.startup-server");
  if (!slot) {
    return std::unexpected(slot.error());
  }

  // Hook the slot.
  auto hook = VtableSlotHook::Install(service, *slot, reinterpret_cast<void*>(&StartupServerThunk));
  if (!hook) {
    return std::unexpected(hook.error());
  }

  // Keep the original and the callbacks for the thunk.
  g_startup_server_original = hook->Original();
  g_before_startup = std::move(before_startup);
  g_after_startup = std::move(after_startup);
  g_before_shutdown = std::move(before_shutdown);
  g_on_error = std::move(on_error);
  return StartupServerProbe(ThunkOwner(std::move(*hook), &ClearThunkState));
#else
  return std::unexpected("the startup server probe requires the Windows host build");
#endif
}

}  // namespace modlock::gameinterop
