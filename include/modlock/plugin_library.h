#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

#include "modlock/build.h"
#include "modlock/export.h"
#include "modlock/plugin.h"

#if defined(_WIN32)
#define MODLOCK_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define MODLOCK_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace modlock {

class EngineHost;
class WasmHost;

namespace net {
struct LaunchConfig;
}

// PluginContext borrows the host and launch arguments for the plugin lifetime.
// The host outlives every plugin and dispatches callbacks on the engine thread.
struct PluginContext {
  EngineHost* engine = nullptr;
  int argc = 0;
  const char* const* argv = nullptr;
  // CheckOnly is true when the host loads, starts and stops the plugin only
  // to check it; the engine never opens a game.
  bool check_only = false;
  const net::LaunchConfig* launch = nullptr;
  // Wasm runs the sandboxed mods a plugin loads, such as a game's modes. The
  // host's interface controller sees their interfaces as it sees the host's
  // own mods. It is null when the host runs no sandboxed mods.
  WasmHost* wasm = nullptr;
  // Hosts upgrading a format that ignored service declarations can retain that
  // admission policy. Calls to unavailable services still fail normally.
  bool enforce_required_services = true;
  // ConcurrentMods runs each sandboxed mod's events on a thread of its own,
  // while every call the mod makes into the game still runs on the engine
  // thread. Off, a mod runs on the engine thread. The host reads it as it loads
  // a mod.
  bool concurrent_mods = false;
  // PostsInPlace lets a mod on its own thread run an event that needs no answer
  // at once on the engine thread, when the mod is idle, instead of queueing it,
  // so a decision that follows is answered by the mod and not by its default.
  // It matters only with concurrent_mods.
  bool posts_in_place = true;
  // ReplayFrames is how many windows a read stays in a mod's replay set after
  // the mod stops making it. It matters only with concurrent_mods.
  uint32_t replay_frames = 1;
};

// PluginManifest is inspected before invoking any C++ plugin implementation.
// The ABI string includes the SDK revision, compiler, platform, and build mode.
struct PluginManifest {
  uint32_t interface_version;
  const char* sdk_abi;
};

// LoadPluginLibrary validates the versioned C entry points and exact SDK ABI.
// The returned plugin owns its library, destroys the native instance through
// that library's destroy function, and then releases the library handle.
[[nodiscard]] MODLOCK_API std::expected<std::unique_ptr<Plugin>, std::string> LoadPluginLibrary(
    const std::filesystem::path& path, const PluginContext& context);

}  // namespace modlock

// Every plugin exports these three symbols with the signatures below.
MODLOCK_PLUGIN_EXPORT modlock::PluginManifest ModlockPluginManifest_v1();
MODLOCK_PLUGIN_EXPORT modlock::Plugin* ModlockPluginCreate_v1(const modlock::PluginContext*);
MODLOCK_PLUGIN_EXPORT void ModlockPluginDestroy_v1(modlock::Plugin*);

// MODLOCK_PLUGIN defines the three symbols for Type, a modlock::Plugin built
// from a const PluginContext&. Use it once, at namespace scope, in one source.
#define MODLOCK_PLUGIN(Type)                                                    \
  MODLOCK_PLUGIN_EXPORT modlock::PluginManifest ModlockPluginManifest_v1() {    \
    return {modlock::PluginInterfaceVersion, MODLOCK_SDK_ABI};                  \
  }                                                                             \
  MODLOCK_PLUGIN_EXPORT modlock::Plugin* ModlockPluginCreate_v1(                \
      const modlock::PluginContext* context) {                                  \
    return context ? new Type(*context) : nullptr;                              \
  }                                                                             \
  MODLOCK_PLUGIN_EXPORT void ModlockPluginDestroy_v1(modlock::Plugin* plugin) { \
    delete plugin;                                                              \
  }
