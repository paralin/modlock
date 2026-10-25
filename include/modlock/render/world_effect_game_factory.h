#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/keyvalues.h"
#include "modlock/render/world_effect_seam.h"
#include "modlock/render/world_particle.h"

namespace modlock::render {

// WorldEffectGameCalls is the source-matched entity lifecycle used by native
// particle and model effects. The entity system is wired once a world is
// ready; no raw world pointer is retained after invalidation.
struct MODLOCK_API WorldEffectGameCalls {
  void* (*create_entity_by_name)(void* ignored_this, const char* class_name,
                                 int force_edict_index) = nullptr;
  void* entity_system = nullptr;
  void (*queue_spawn_entity)(void* entity_system, void* identity, void* key_values) = nullptr;
  void (*execute_queued_creation)(void* entity_system) = nullptr;
  bool (*accept_input)(void* entity, const char* input_name, void* activator, void* caller,
                       void* value, int output_id, void* unknown) = nullptr;
  modlock::gameinterop::KeyValuesCalls key_values;
  void (*util_remove)(void* entity) = nullptr;
  // set_abs_origin and set_abs_angles move a spawned entity without a warp:
  // the physics step pulls a body the game does not simulate to its new place
  // over one tick, so a solid model moved this way strikes with its speed.
  void (*set_abs_origin)(void* entity, const float* origin) = nullptr;
  void (*set_abs_angles)(void* entity, const float* angles) = nullptr;
};

// ParticleFields resolves the five schema fields used by native particles.
// Resolution happens after the server's schema classes are available.
struct ParticleFields {
  gameinterop::SchemaField tint;
  gameinterop::SchemaField tint_control_point;
  gameinterop::SchemaField data_control_point;
  gameinterop::SchemaField data;
  gameinterop::SchemaField control_points;
};

// WorldEffectGameFactory creates the shipped native particle entities through
// CreateEntityByName, keyvalues, Teleport, queued creation, Start, and remove.
class MODLOCK_API WorldEffectGameFactory final : public WorldEffectFactory {
 public:
  static std::expected<std::unique_ptr<WorldEffectGameFactory>, std::string> TryCreate(
      const modlock::gameinterop::ModuleImage& server);

  explicit WorldEffectGameFactory(WorldEffectGameCalls calls);
  ~WorldEffectGameFactory() override;

  // SetEntitySystem supplies the current world entity system after readiness.
  void SetEntitySystem(void* entity_system) { calls_.entity_system = entity_system; }

  // Sweep removes each retired particle entity whose end cap has had time to
  // play. It runs once per frame on the engine thread.
  void Sweep(std::chrono::steady_clock::time_point now);

  // InvalidateAfterEngineReset forgets the retired entities of a world that
  // ended, without removing them.
  void InvalidateAfterEngineReset() { retired_.clear(); }

  std::expected<std::unique_ptr<WorldEffect>, std::string> Create(
      const ParticleSettings& settings) override;

  std::expected<std::unique_ptr<WorldEffect>, std::string> CreateModel(
      const WorldModelSettings& settings) override;

  // CreateParticle accepts a game-relative particle resource and the complete
  // native spawn settings. The engine resolves the resource through keyvalues.
  std::expected<std::unique_ptr<WorldParticle>, std::string> CreateParticle(
      const ParticleSettings& settings);

  // CreateFogController enables spatial volumetric fog; its volumes determine
  // where fog exists. Remove volumes before removing their controller.
  std::expected<std::unique_ptr<WorldEffect>, std::string> CreateFogController(
      const geometry::Vec3& origin, float draw_distance);

  // CreateFogVolume creates an oriented box of tinted fog. Bounds are
  // local to origin, and density fades across the box's boundary.
  std::expected<std::unique_ptr<WorldEffect>, std::string> CreateFogVolume(
      const geometry::Vec3& origin, const std::array<float, 3>& angles,
      const std::array<float, 3>& mins, const std::array<float, 3>& maxs, float strength,
      const std::array<uint8_t, 3>& tint);

 private:
  friend class GameWorldEffect;
  friend class GameWorldEntity;

  std::expected<std::unique_ptr<WorldEffect>, std::string> CreateEntity(
      const char* class_name, const geometry::Vec3& origin, const std::array<float, 3>& angles,
      std::span<const gameinterop::EntityKeyValue> properties);

  void TraceFailure(std::string_view message);
  void TraceSuccess();
  std::expected<const ParticleFields*, std::string> ResolveParticleFields();

  // Retire removes a stopped particle entity once its end cap has played. The
  // client keeps a stopped system's particles until its end cap decays them,
  // so the entity must outlive the stop by at least one network update.
  void Retire(gameinterop::EntityRef entity);

  // Retired is a particle entity waiting for its removal time.
  struct Retired {
    gameinterop::EntityRef entity;
    std::chrono::steady_clock::time_point remove_at;
  };

  WorldEffectGameCalls calls_;
  std::optional<ParticleFields> particle_fields_;
  std::vector<Retired> retired_;
  std::set<std::string> trace_failures_;
  bool trace_success_logged_ = false;
};

}  // namespace modlock::render
