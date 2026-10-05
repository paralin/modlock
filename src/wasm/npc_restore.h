#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "modlock/gameinterop/world_entities.h"

namespace modlock::wasm {

// NpcRestore gives the map a recorded set of lane units and objectives over
// three frames: it replaces them, lets the game initialize the new units,
// places them, then reads the map back and matches every target to one unit
// of its class, data entry, team, health and lane within 16 units of its
// position. It holds no engine pointer.
class NpcRestore {
 public:
  using Target = gameinterop::WorldEntities::Target;
  using Sample = gameinterop::WorldEntities::Sample;
  using Result = std::optional<std::expected<void, std::string>>;

  // Engine is the world access a restore uses.
  struct Engine {
    std::function<std::expected<void, std::string>(std::span<const Target>)> restore;
    std::function<std::expected<void, std::string>()> finish;
    std::function<std::expected<std::vector<Sample>, std::string>()> read;
  };

  // Create checks targets: each needs a whole positive health and maximum,
  // and each trooper a lane.
  static std::expected<NpcRestore, std::string> Create(std::vector<Target> targets, Engine engine);

  // Tick advances the restore by one frame. It returns nothing while the
  // restore runs and its result once it ends.
  Result Tick();

  // Match reports the first target samples do not hold, or nothing when each
  // target matches its own unit and no unit is left over.
  static std::optional<std::string> Match(std::span<const Target> targets,
                                          std::span<const Sample> samples);

 private:
  enum class Phase { kRestore, kFinish, kVerify };

  NpcRestore(std::vector<Target> targets, Engine engine);

  std::vector<Target> targets_;
  Engine engine_;
  Phase phase_ = Phase::kRestore;
};

}  // namespace modlock::wasm
