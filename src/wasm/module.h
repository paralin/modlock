#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "modlock/gameinterop/world_entities.h"

namespace modlock::wasm {

// Module is one domain of a mod's game: it owns that domain's state and its
// part of the game's lifecycle. Game calls every hook on every module it
// holds, so a module that keeps state releases it by overriding the hook
// rather than by Game remembering to. Everything runs on the engine thread.
class Module {
 public:
  virtual ~Module() = default;

  // Frame runs once per server frame while the mod runs.
  virtual void Frame() {}

  // WorldReady runs when a map has loaded.
  virtual void WorldReady() {}

  // WorldEnding runs before the world goes; the module drops what the world
  // held and keeps what follows the mod into the next world.
  virtual void WorldEnding() {}

  // Leave runs when the player in slot left, so the next player there starts
  // clear.
  virtual void Leave(int32_t) {}

  // Clear releases everything the mod held through this module, as the mod
  // stops or is replaced.
  virtual void Clear() {}
};

// WorldSource gives a module the loaded world's entity operations without
// naming the module that owns them.
class WorldSource {
 public:
  // World returns this world's entity operations, building them on first use.
  virtual std::expected<gameinterop::WorldEntities*, std::string> World() = 0;

 protected:
  ~WorldSource() = default;
};

}  // namespace modlock::wasm
