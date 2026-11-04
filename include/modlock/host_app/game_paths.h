#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include "modlock/export.h"

namespace modlock::host_app {

// GamePaths holds the resolved locations the host needs from an installed
// Deadlock. Server modules live one level deeper than the engine library.
struct GamePaths {
  // root is the Deadlock installation supplied to ResolveGamePaths.
  std::filesystem::path root;
  // engine2_dll resolves under <game>/game/bin/win64/engine2.dll.
  std::filesystem::path engine2_dll;
  // server_dll resolves under <game>/game/citadel/bin/win64/server.dll.
  std::filesystem::path server_dll;
};

// ResolveGamePaths lays out the expected module paths for a game install
// root (the DEADLOCK_DIR convention). Paths are computed even when files
// are absent; Validate reports the first missing one.
[[nodiscard]] MODLOCK_API GamePaths ResolveGamePaths(const std::filesystem::path& game_dir);

// Validate returns an empty string when every module file exists, otherwise
// a diagnostic naming the first missing file and the fix.
[[nodiscard]] MODLOCK_API std::string Validate(const GamePaths& paths);

}  // namespace modlock::host_app
