#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include "modlock/export.h"

namespace modlock::net {

// LaunchConfig selects the engine role. By default the process is a dedicated
// server on host_port running map; a nonempty connect instead runs a game
// client that joins that address and ignores the server fields.
//
// Clients join the default port with `connect localhost:27067`.
// This keeps clear of UDP 27015, which the stock client binds for its own
// embedded listen server
// and access-violates on when held by another process (three minidumps,
// 2026-08-25).
struct LaunchConfig {
  uint16_t host_port = 27067;
  std::string map = "dl_midtown";
  std::string connect;
  // engine_arguments are appended to either role's command line unchanged.
  std::string engine_arguments;
};

// BuildDedicatedCommandLine renders the engine command line that starts the
// already-loaded modules as a listening dedicated server. Pure text; the
// engine parses it during the handoff.
[[nodiscard]] MODLOCK_API std::string BuildDedicatedCommandLine(const LaunchConfig& config);

// BuildClientCommandLine renders the engine command line of a game client that
// joins config.connect. The client reads and writes the player's own settings;
// -insecure lets it join the host's insecure server and keeps it off
// VAC-secured ones.
[[nodiscard]] MODLOCK_API std::string BuildClientCommandLine(const LaunchConfig& config);

// Source2Main is the exported engine entry point (engine2.dll). Once called,
// the engine owns the process: it loads the map, opens the UDP host port, and
// runs its own frame loop until shutdown.
using Source2MainFn = int (*)(void* hInstance, void* hPrevInstance, const char* pszCmdLine,
                              int nShowCmd, const char* pszBaseDir, const char* pszGame);

// RunEngine hands the process to the engine in the configured role. The game
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
