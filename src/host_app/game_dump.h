#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>

#include "modlock/host_app/game_paths.h"

namespace modlock::host_app {

// GameDumpCounts reports how much a dump found.
struct GameDumpCounts {
  size_t classes = 0;
  size_t enums = 0;
  size_t designer_names = 0;
  size_t data_maps = 0;
  size_t inputs = 0;
  size_t outputs = 0;
  size_t variables = 0;
  size_t commands = 0;
};

// WriteGameDump describes the running server as the game describes it to
// itself and writes schemas.json, entities.json and console.json to
// directory. Call it on the engine thread once the world is ready, so every
// module has registered its schema, entity classes and console entries.
[[nodiscard]] std::expected<GameDumpCounts, std::string> WriteGameDump(
    const GamePaths& paths, const std::filesystem::path& directory);

}  // namespace modlock::host_app
