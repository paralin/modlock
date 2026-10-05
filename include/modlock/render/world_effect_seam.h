#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modlock/export.h"
#include "proto/modlock/types.pb.h"

namespace modlock::render {

struct ParticleTint {
  std::uint32_t rgba = UINT32_MAX;
  int control_point = 0;
};

struct ParticleData {
  int control_point = 0;
  std::array<float, 3> value{};
};

// ParticleSettings is the complete native info_particle_system spawn recipe.
// Entity references are serial-fenced handles resolved in the current world.
struct ParticleSettings {
  std::string resource;
  Vec3 origin;
  std::array<float, 3> angles{};
  bool start_active = true;
  std::optional<ParticleTint> tint;
  std::optional<ParticleData> data;
  std::vector<std::pair<int, std::uint32_t>> control_points;
  std::optional<std::uint32_t> parent;
};

// WorldModelSettings describes a prop. It has no NPC behavior. A solid prop
// collides through the model's physics shape, scaled with the model.
struct WorldModelSettings {
  std::string resource;
  Vec3 origin;
  std::array<float, 3> angles{};
  float scale = 1;
  std::uint32_t color_rgba = UINT32_MAX;
  bool glow = false;
  bool solid = false;
};

// WorldEffect is one live visual entity handle. The handle is non-owning
// with respect to the engine and must be invalidated at a world replacement.
class MODLOCK_API WorldEffect {
 public:
  virtual ~WorldEffect() = default;

  // Move updates the effect origin through the entity's proven Teleport slot,
  // turning it to angles when given.
  virtual void Move(const modlock::Vec3& origin,
                    const std::optional<std::array<float, 3>>& angles) = 0;

  // Handle returns the entity's packed handle while it is live.
  virtual std::optional<std::uint32_t> Handle() const = 0;

  // Remove queues this effect for destruction exactly once.
  virtual void Remove() = 0;

  // InvalidateAfterEngineReset drops the handle without an engine call.
  virtual void InvalidateAfterEngineReset() {}
};

// WorldEffectFactory creates native particles and props. Raw game ABI calls
// remain inside its implementation.
class MODLOCK_API WorldEffectFactory {
 public:
  virtual ~WorldEffectFactory() = default;

  // Create validates the shipped guidance resource and starts one particle
  // effect from settings at its origin, returning its handle.
  virtual std::expected<std::unique_ptr<WorldEffect>, std::string> Create(
      const ParticleSettings& settings) = 0;

  // CreateModel spawns a persistent prop, solid when settings ask.
  virtual std::expected<std::unique_ptr<WorldEffect>, std::string> CreateModel(
      const WorldModelSettings& settings) = 0;
};

}  // namespace modlock::render
