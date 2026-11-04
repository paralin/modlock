#include "modlock/host_app/game_paths.h"

#include "modlock/host_app/module_loader.h"

namespace modlock::host_app {

GamePaths ResolveGamePaths(const std::filesystem::path& game_dir) {
  // engine2.dll sits in the common bin directory; the game server modules
  // live one level deeper under citadel (recorded live-build layout).
  return GamePaths{
      .root = game_dir,
      .engine2_dll = game_dir / "game" / "bin" / "win64" / "engine2.dll",
      .server_dll = game_dir / "game" / "citadel" / "bin" / "win64" / "server.dll",
  };
}

std::string Validate(const GamePaths& paths) {
  if (!std::filesystem::exists(paths.engine2_dll)) {
    return "engine2.dll not found at " + paths.engine2_dll.string() +
           "; set DEADLOCK_DIR to the Deadlock install root";
  }
  if (!std::filesystem::exists(paths.server_dll)) {
    return "server.dll not found at " + paths.server_dll.string() +
           "; set DEADLOCK_DIR to the Deadlock install root";
  }
  return {};
}

std::vector<PlannedLoad> PlanModuleLoads(const GamePaths& paths) {
  return {
      PlannedLoad{.name = "engine2.dll", .path = paths.engine2_dll},
      PlannedLoad{.name = "server.dll", .path = paths.server_dll},
  };
}

}  // namespace modlock::host_app
