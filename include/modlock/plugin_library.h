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
  bool check_only = false;
  const net::LaunchConfig* launch = nullptr;
  // Wasm runs the sandboxed mods a plugin loads, such as a game's modes. The
  // host's interface controller sees their interfaces as it sees the host's
  // own mods. It is null when the host runs no sandboxed mods.
  WasmHost* wasm = nullptr;
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
