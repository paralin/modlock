#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include "modlock/export.h"

namespace modlock::net {

// LaunchConfig configures the dedicated server the process runs: it serves
// map on host_port.
//
// Clients join the default port with `connect localhost:27067`.
// This keeps clear of UDP 27015, which the stock client binds for its own
// embedded listen server
// and access-violates on when held by another process (three minidumps,
// 2026-08-25).
struct LaunchConfig {
  uint16_t host_port = 27067;
  std::string map = "dl_midtown";
  // engine_arguments are appended to the command line unchanged.
  std::string engine_arguments;
};

// BuildDedicatedCommandLine renders the engine command line that starts the
// already-loaded modules as a listening dedicated server. Pure text; the
// engine parses it during the handoff.
[[nodiscard]] MODLOCK_API std::string BuildDedicatedCommandLine(const LaunchConfig& config);

// Source2Main is the exported engine entry point (engine2.dll). Once called,
// the engine owns the process: it loads the map, opens the UDP host port, and
// runs its own frame loop until shutdown.
using Source2MainFn = int (*)(void* hInstance, void* hPrevInstance, const char* pszCmdLine,
                              int nShowCmd, const char* pszBaseDir, const char* pszGame);

// RunEngine hands the process to the engine as a dedicated server. The game
// modules must already be mapped by the host app (Windows only);
// engine_bin_dir is the directory holding engine2.dll, game/bin/win64. The
// engine resolves citadel and core relative to pszBaseDir, so RunEngine passes
// the game directory two levels up, as the stock launcher does.
// Each precondition is checked in dependency order and a failure names the
// aborted stage. Blocks until the engine exits and yields its exit code; an
// error means the engine never started.
[[nodiscard]] MODLOCK_API std::expected<int, std::string> RunEngine(
    const LaunchConfig& config, const std::filesystem::path& engine_bin_dir);

}  // namespace modlock::net
