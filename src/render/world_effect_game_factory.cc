#include "modlock/render/world_effect_game_factory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "modlock/gameinterop/entity_abi.h"

namespace modlock::render {
namespace {

using gameinterop::InteropTraceEnabled;

constexpr size_t kMaxTraceFailures = 32;
constexpr const char* kShippedEffects[] = {
    "particles/environment/powerup_spawner_ambient.vpcf",
    "particles/environment/powerup_spawner_ambient_beam.vpcf",
    "particles/environment/powerup_spawner_ambient_ring.vpcf",
    "particles/environment/powerup_spawner_ambient_light.vpcf",
    "particles/environment/powerup_spawner_ambient_spill.vpcf",
    "particles/environment/powerup_spawner_ambient_magic.vpcf",
    "particles/generic/generic_ping_ground_arrow.vpcf",
    "particles/abilities/assassinate_laser_targetting.vpcf",
};

bool IsShippedEffect(std::string_view effect_name) {
  for (const char* shipped : kShippedEffects) {
    if (effect_name == shipped) {
      return true;
    }
  }
  return false;
}

// Teleport places an effect entity at origin with angles and no motion.
void Teleport(void* entity, const modlock::geometry::Vec3& origin,
              const std::array<float, 3>& angles) {
  gameinterop::TeleportEntity(entity,
                              {static_cast<float>(origin.x()), static_cast<float>(origin.y()),
                               static_cast<float>(origin.z())},
                              angles, {});
}

}  // namespace

std::expected<std::unique_ptr<WorldEffectGameFactory>, std::string>
WorldEffectGameFactory::TryCreate(const modlock::gameinterop::ModuleImage& server) {
  // Resolve each engine call by its signature.
  WorldEffectGameCalls calls;

  // Fill every call, or fail at the first missing one.
  const modlock::gameinterop::SignatureCall signatures[] = {
      {"entity-system.create-entity-by-name", &calls.create_entity_by_name},
      {"entity-system.queue-spawn-entity", &calls.queue_spawn_entity},
      {"entity-system.execute-queued-creation", &calls.execute_queued_creation},
      {"entity-instance.accept-input", &calls.accept_input},
      {"entity.remove", &calls.util_remove},
      {"entity.set-abs-origin", &calls.set_abs_origin},
      {"entity.set-abs-angles", &calls.set_abs_angles},
  };
  auto resolved = modlock::gameinterop::ResolveSignatures(server, signatures);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  // Resolve the keyvalue calls.
  auto key_values = modlock::gameinterop::ResolveKeyValuesCalls(server);
  if (!key_values) {
    return std::unexpected(key_values.error());
  }
  calls.key_values = *key_values;
  return std::unique_ptr<WorldEffectGameFactory>(new WorldEffectGameFactory(calls));
}

WorldEffectGameFactory::WorldEffectGameFactory(WorldEffectGameCalls calls) : calls_(calls) {}

// kEndCapTime is how long a retired particle entity outlives its stop, so each
// client receives the stop and plays the end cap before the entity goes.
constexpr auto kEndCapTime = std::chrono::seconds(2);

WorldEffectGameFactory::~WorldEffectGameFactory() {
  Sweep(std::chrono::steady_clock::time_point::max());
}

void WorldEffectGameFactory::Retire(gameinterop::EntityRef entity) {
  retired_.push_back({entity, std::chrono::steady_clock::now() + kEndCapTime});
}

void WorldEffectGameFactory::Sweep(std::chrono::steady_clock::time_point now) {
  std::erase_if(retired_, [&](const Retired& retired) {
    if (retired.remove_at > now) {
      return false;
    }
    if (void* entity = retired.entity.Get()) {
      calls_.util_remove(entity);
    }
    return true;
  });
}

class GameWorldEffect final : public WorldParticle {
 public:
  GameWorldEffect(WorldEffectGameFactory& factory, ParticleSettings settings)
      : factory_(factory), settings_(std::move(settings)) {}

  ~GameWorldEffect() override { Remove(); }

  void Move(const modlock::geometry::Vec3& origin,
            const std::optional<std::array<float, 3>>& angles) override {
    if (!entity_.Get()) {
      return;
    }
    (void)Transform(origin, angles.value_or(settings_.angles));
  }

  std::optional<std::uint32_t> Handle() const override { return entity_.Handle(); }

  std::expected<void, std::string> Start() override { return Input("Start"); }

  // Stop plays the end cap; the plain Stop input leaves particles that decay
  // only on their end cap frozen in place.
  std::expected<void, std::string> Stop() override { return Input("StopPlayEndCap"); }

  std::expected<void, std::string> Transform(const geometry::Vec3& origin,
                                             const std::array<float, 3>& angles) override {
    // Only a live particle with a finite pose moves.
    void* entity = entity_.Get();
    if (!entity) {
      return std::unexpected("particle is no longer live");
    }
    if (!std::isfinite(origin.x()) || !std::isfinite(origin.y()) || !std::isfinite(origin.z()) ||
        !std::ranges::all_of(angles, [](float v) { return std::isfinite(v); })) {
      return std::unexpected("particle transform is not finite");
    }
    Teleport(entity, origin, angles);
    settings_.origin = origin;
    settings_.angles = angles;
    return {};
  }

  std::expected<void, std::string> Tint(const ParticleTint& tint) override {
    // Only a live particle takes a tint, at a control point the effect has.
    if (!entity_.Get()) {
      return std::unexpected("particle is no longer live");
    }
    if (tint.control_point < 0 || tint.control_point >= 64) {
      return std::unexpected("particle tint control point must be between 0 and 63");
    }
    auto fields = factory_.ResolveParticleFields();
    if (!fields) {
      return std::unexpected(fields.error());
    }
    if (auto wrote = Write((*fields)->tint, tint.rgba); !wrote) {
      return wrote;
    }
    return Write((*fields)->tint_control_point, tint.control_point);
  }

  std::expected<void, std::string> Data(const ParticleData& data) override {
    // Only a live particle takes finite data, at a control point the effect has.
    if (!entity_.Get()) {
      return std::unexpected("particle is no longer live");
    }
    if (data.control_point < 0 || data.control_point >= 64 ||
        !std::ranges::all_of(data.value, [](float v) { return std::isfinite(v); })) {
      return std::unexpected("particle data control point or value is invalid");
    }
    auto fields = factory_.ResolveParticleFields();
    if (!fields) {
      return std::unexpected(fields.error());
    }
    if (auto wrote = Write((*fields)->data_control_point, data.control_point); !wrote) {
      return wrote;
    }
    return Write((*fields)->data, data.value);
  }

  std::expected<void, std::string> ControlPoint(int index, std::uint32_t handle) override {
    // Only a live particle takes a control point.
    if (!entity_.Get()) {
      return std::unexpected("particle is no longer live");
    }
    auto fields = factory_.ResolveParticleFields();
    if (!fields) {
      return std::unexpected(fields.error());
    }

    // The index must fall inside the native array, and the entity must be live.
    auto field = (*fields)->control_points;
    if (index < 0 || static_cast<size_t>(index) >= field.size / sizeof(handle)) {
      return std::unexpected("particle control point index is outside the native array");
    }
    if (handle != UINT32_MAX &&
        !gameinterop::EntityInstance(factory_.calls_.entity_system, handle)) {
      return std::unexpected("particle control point entity is no longer live");
    }

    // Write the one handle.
    field.offset += static_cast<size_t>(index) * sizeof(handle);
    field.size = sizeof(handle);
    return Write(field, handle);
  }

  std::expected<void, std::string> Attach(std::uint32_t parent_handle) override {
    if (!entity_.Get()) {
      return std::unexpected("particle is no longer live");
    }
    void* parent = gameinterop::EntityInstance(factory_.calls_.entity_system, parent_handle);
    if (!parent) {
      return std::unexpected("particle parent is no longer live");
    }
    return Input("SetParent", parent, "!activator");
  }

  std::expected<void, std::string> Detach() override { return Input("ClearParent"); }

  // Remove stops the particle with its end cap and leaves the entity to the
  // factory, which removes it once the end cap has played.
  void Remove() override {
    if (entity_.Get()) {
      static_cast<void>(Stop());
      factory_.Retire(entity_);
    }
    entity_.Reset();
  }

  // Discard removes an entity that never finished spawning.
  void Discard() {
    if (void* entity = entity_.Get()) {
      factory_.calls_.util_remove(entity);
    }
    entity_.Reset();
  }

  void InvalidateAfterEngineReset() override { entity_.Reset(); }

  std::expected<void, std::string> Spawn() {
    // Every engine call must be resolved.
    if (factory_.calls_.create_entity_by_name == nullptr) {
      return std::unexpected("effect: create-entity-by-name unresolved");
    }
    if (factory_.calls_.entity_system == nullptr || factory_.calls_.queue_spawn_entity == nullptr ||
        factory_.calls_.execute_queued_creation == nullptr ||
        factory_.calls_.accept_input == nullptr || factory_.calls_.util_remove == nullptr) {
      return std::unexpected(
          "effect: creation unresolved (entity system, queue, execute, "
          "accept-input, or remove)");
    }

    // Create the entity, hold it by handle, and build its keyvalues.
    void* entity = factory_.calls_.create_entity_by_name(nullptr, "info_particle_system", -1);
    if (entity == nullptr) {
      return std::unexpected("create info_particle_system returned null");
    }
    entity_ = gameinterop::EntityRef(factory_.calls_.entity_system, entity);
    if (!entity_.Get()) {
      factory_.calls_.util_remove(entity);
      return std::unexpected("info_particle_system has no live handle");
    }
    const modlock::gameinterop::EntityKeyValue pairs[] = {
        {.key = "effect_name", .value = std::string_view(settings_.resource)},
        {.key = "start_active", .value = settings_.start_active},
    };
    auto built = modlock::gameinterop::BuildEntityKeyValues(factory_.calls_.key_values, pairs);
    if (!built) {
      return std::unexpected("effect: " + built.error());
    }

    // Apply the tint, data and control points.
    if (settings_.tint) {
      if (auto applied = Tint(*settings_.tint); !applied) {
        return applied;
      }
    }
    if (settings_.data) {
      if (auto applied = Data(*settings_.data); !applied) {
        return applied;
      }
    }
    for (const auto& [index, handle] : settings_.control_points) {
      if (auto applied = ControlPoint(index, handle); !applied) {
        return applied;
      }
    }

    // Place it, then spawn it.
    if (auto moved = Transform(settings_.origin, settings_.angles); !moved) {
      return moved;
    }
    factory_.calls_.queue_spawn_entity(factory_.calls_.entity_system,
                                       modlock::gameinterop::IdentityOf(entity), *built);
    factory_.calls_.execute_queued_creation(factory_.calls_.entity_system);

    // Start and attach it as configured.
    if (settings_.start_active) {
      if (auto started = Start(); !started) {
        return started;
      }
    }
    if (settings_.parent) {
      if (auto attached = Attach(*settings_.parent); !attached) {
        return attached;
      }
    }
    return {};
  }

 private:
  std::expected<void, std::string> Input(const char* name, void* activator = nullptr,
                                         const char* value = "") {
    void* entity = entity_.Get();
    if (!entity) {
      return std::unexpected("particle is no longer live");
    }
    gameinterop::Variant variant{.pointer = value, .type = gameinterop::VariantType::kCString};
    if (!factory_.calls_.accept_input(entity, name, activator, nullptr, &variant, 0, nullptr)) {
      return std::unexpected(std::string("info_particle_system rejected ") + name);
    }
    return {};
  }

  template <typename T>
  std::expected<void, std::string> Write(const gameinterop::SchemaField& field, const T& value) {
    // Only a live particle takes a write that fits its field.
    void* entity = entity_.Get();
    if (!entity) {
      return std::unexpected("particle is no longer live");
    }
    if (field.size < sizeof(T)) {
      return std::unexpected("particle schema field is too small");
    }

    // Write it, notifying the network when the field is networked.
    std::memcpy(static_cast<std::byte*>(entity) + field.offset, &value, sizeof(value));
    if (field.networked && !gameinterop::NotifyEntityStateChanged(entity)) {
      return std::unexpected("particle network state notification unavailable");
    }
    return {};
  }

  WorldEffectGameFactory& factory_;
  ParticleSettings settings_;
  gameinterop::EntityRef entity_;
};

// GameWorldEntity owns a visual entity until removal or world invalidation.
class GameWorldEntity final : public WorldEffect {
 public:
  GameWorldEntity(WorldEffectGameFactory& factory, gameinterop::EntityRef entity,
                  std::array<float, 3> angles)
      : factory_(factory), entity_(entity), angles_(angles) {}

  ~GameWorldEntity() override { Remove(); }

  void Move(const geometry::Vec3& origin,
            const std::optional<std::array<float, 3>>& angles) override {
    void* entity = entity_.Get();
    if (!entity) {
      return;
    }
    if (angles) {
      angles_ = *angles;
    }
    const float position[] = {static_cast<float>(origin.x()), static_cast<float>(origin.y()),
                              static_cast<float>(origin.z())};
    factory_.calls_.set_abs_origin(entity, position);
    factory_.calls_.set_abs_angles(entity, angles_.data());
  }

  std::optional<std::uint32_t> Handle() const override { return entity_.Handle(); }

  void Remove() override {
    if (void* entity = entity_.Get()) {
      factory_.calls_.util_remove(entity);
    }
    entity_.Reset();
  }

  void InvalidateAfterEngineReset() override { entity_.Reset(); }

 private:
  WorldEffectGameFactory& factory_;
  gameinterop::EntityRef entity_;
  std::array<float, 3> angles_;
};

std::expected<std::unique_ptr<WorldEffect>, std::string> WorldEffectGameFactory::CreateEntity(
    const char* class_name, const geometry::Vec3& origin, const std::array<float, 3>& angles,
    std::span<const gameinterop::EntityKeyValue> properties) {
  // Every engine call must be resolved.
  if (!calls_.entity_system || !calls_.create_entity_by_name || !calls_.queue_spawn_entity ||
      !calls_.execute_queued_creation || !calls_.util_remove) {
    return std::unexpected("visual entity: native entity creation is unavailable");
  }

  // Build the keyvalues with the classname a map would give, and create the
  // entity. Spawn reads the key, not the factory: prop_dynamic_override marks
  // itself an override prop only when its classname key names it.
  std::vector<gameinterop::EntityKeyValue> keyed;
  keyed.reserve(properties.size() + 1);
  keyed.push_back({.key = "classname", .value = std::string_view(class_name)});
  keyed.insert(keyed.end(), properties.begin(), properties.end());
  auto values = gameinterop::BuildEntityKeyValues(calls_.key_values, keyed);
  if (!values) {
    return std::unexpected(std::string(class_name) + ": " + values.error());
  }
  auto* entity = calls_.create_entity_by_name(nullptr, class_name, -1);
  if (!entity) {
    return std::unexpected(std::string("create ") + class_name + " returned null");
  }

  // Hold it by handle, so a removal by the engine leaves nothing dangling.
  const gameinterop::EntityRef ref(calls_.entity_system, entity);
  if (!ref.Get()) {
    calls_.util_remove(entity);
    return std::unexpected(std::string(class_name) + " has no live handle");
  }

  // Place it, then spawn it.
  auto effect = std::make_unique<GameWorldEntity>(*this, ref, angles);
  Teleport(entity, origin, angles);
  calls_.queue_spawn_entity(calls_.entity_system, gameinterop::IdentityOf(entity), *values);
  calls_.execute_queued_creation(calls_.entity_system);
  return effect;
}

std::expected<std::unique_ptr<WorldEffect>, std::string>
WorldEffectGameFactory::CreateFogController(const geometry::Vec3& origin, float draw_distance) {
  const gameinterop::EntityKeyValue properties[] = {
      {.key = "IsMaster", .value = true},
      {.key = "startdisabled", .value = false},
      {.key = "FogStrength", .value = 1.f},
      {.key = "DrawDistance", .value = draw_distance},
      {.key = "FadeInStart", .value = 0.f},
      {.key = "FadeInEnd", .value = 1.f},
      {.key = "FadeSpeed", .value = 0.f},
      {.key = "TintColor", .value = gameinterop::KeyValueColor{0, 0, 0, 255}},
      {.key = "IndirectEnabled", .value = false},
  };
  return CreateEntity("env_volumetric_fog_controller", origin, {}, properties);
}

std::expected<std::unique_ptr<WorldEffect>, std::string> WorldEffectGameFactory::CreateFogVolume(
    const geometry::Vec3& origin, const std::array<float, 3>& angles,
    const std::array<float, 3>& mins, const std::array<float, 3>& maxs, float strength,
    const std::array<uint8_t, 3>& tint) {
  const gameinterop::EntityKeyValue properties[] = {
      {.key = "startdisabled", .value = false},
      {.key = "box_mins", .value = gameinterop::KeyValueVector{mins[0], mins[1], mins[2]}},
      {.key = "box_maxs", .value = gameinterop::KeyValueVector{maxs[0], maxs[1], maxs[2]}},
      {.key = "FogStrength", .value = strength},
      {.key = "Shape", .value = 0},
      {.key = "FalloffExponent", .value = 0.2f},
      {.key = "OverrideTintColor", .value = true},
      {.key = "TintColor", .value = gameinterop::KeyValueColor{tint[0], tint[1], tint[2], 255}},
      {.key = "OverrideFogIndirectStrength", .value = true},
      {.key = "FogIndirectStrength", .value = 0.f},
      {.key = "OverrideFogSunLightStrength", .value = true},
      {.key = "FogSunLightStrength", .value = 1.f},
  };
  return CreateEntity("env_volumetric_fog_volume", origin, angles, properties);
}

std::expected<std::unique_ptr<WorldEffect>, std::string> WorldEffectGameFactory::CreateModel(
    const WorldModelSettings& settings) {
  // Refuse a resource outside models/ and a bad scale.
  if (!settings.resource.starts_with("models/") || !settings.resource.ends_with(".vmdl") ||
      settings.resource.find("..") != std::string::npos) {
    return std::unexpected("model resource must be a game-relative models/*.vmdl path");
  }
  if (!std::isfinite(settings.scale) || settings.scale <= 0) {
    return std::unexpected("model scale must be finite and positive");
  }

  // The override accepts models with propdata without turning scenery into physics props.
  const auto rgba = settings.color_rgba;
  const gameinterop::KeyValueColor color{
      static_cast<uint8_t>(rgba), static_cast<uint8_t>(rgba >> 8), static_cast<uint8_t>(rgba >> 16),
      static_cast<uint8_t>(rgba >> 24)};
  const gameinterop::EntityKeyValue properties[] = {
      {.key = "model", .value = std::string_view(settings.resource)},
      // Solid 6 collides through the model's physics shape; spawnflag 256
      // starts a prop with collision disabled.
      {.key = "solid", .value = settings.solid ? 6 : 0},
      {.key = "spawnflags", .value = (settings.solid ? 0 : 256) | 512 | 1024},
      {.key = "scales",
       .value = gameinterop::KeyValueVector{settings.scale, settings.scale, settings.scale}},
      // rendercolor carries only red, green and blue; the alpha applies as
      // renderamt, and only under kRenderTransAlpha.
      {.key = "rendercolor", .value = color},
      {.key = "rendermode", .value = color.alpha == 0xff ? 0 : 1},
      {.key = "renderamt", .value = int{color.alpha}},
      {.key = "glowcolor", .value = color},
      {.key = "glowstate", .value = settings.glow ? 3 : 0},
      {.key = "glowrange", .value = 0},
      {.key = "glowteam", .value = -1},
      {.key = "disableshadows", .value = true},
      {.key = "use_animgraph", .value = false},
  };
  return CreateEntity("prop_dynamic_override", settings.origin, settings.angles, properties);
}

std::expected<std::unique_ptr<WorldEffect>, std::string> WorldEffectGameFactory::Create(
    const ParticleSettings& settings) {
  if (!IsShippedEffect(settings.resource)) {
    const std::string error =
        "effect path is not one of the shipped guidance resources: " + settings.resource;
    TraceFailure(error);
    return std::unexpected(error);
  }
  return CreateParticle(settings);
}

std::expected<std::unique_ptr<WorldParticle>, std::string> WorldEffectGameFactory::CreateParticle(
    const ParticleSettings& settings) {
  // Refuse a resource outside particles/ or with stray characters.
  const auto& resource = settings.resource;
  if (!resource.starts_with("particles/") || !resource.ends_with(".vpcf") ||
      resource.find("..") != std::string::npos ||
      !std::ranges::all_of(resource, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '/' || c == '_' || c == '-' || c == '.';
      })) {
    return std::unexpected("particle resource must be a game-relative particles/*.vpcf path");
  }

  // Spawn it, tracing the outcome.
  auto effect = std::make_unique<GameWorldEffect>(*this, settings);
  if (auto spawned = effect->Spawn(); !spawned) {
    effect->Discard();
    TraceFailure(spawned.error());
    return std::unexpected(spawned.error());
  }
  TraceSuccess();
  return effect;
}

std::expected<const ParticleFields*, std::string> WorldEffectGameFactory::ResolveParticleFields() {
  // Resolve the fields once.
  if (particle_fields_) {
    return &*particle_fields_;
  }
  auto schema = gameinterop::ResolveSchemaSystem();
  if (!schema) {
    return std::unexpected(schema.error());
  }

  // Look up each particle field.
  ParticleFields fields;
  const std::pair<const char*, gameinterop::SchemaField*> entries[] = {
      {"m_clrTint", &fields.tint},
      {"m_nTintCP", &fields.tint_control_point},
      {"m_nDataCP", &fields.data_control_point},
      {"m_vecDataCPValue", &fields.data},
      {"m_hControlPointEnts", &fields.control_points},
  };
  for (const auto& [name, destination] : entries) {
    auto field = gameinterop::SchemaFieldOf(*schema, "server.dll", "CParticleSystem", name);
    if (!field) {
      return std::unexpected(field.error());
    }
    *destination = *field;
  }
  particle_fields_ = fields;
  return &*particle_fields_;
}

void WorldEffectGameFactory::TraceFailure(std::string_view message) {
  if (!InteropTraceEnabled() || trace_failures_.size() >= kMaxTraceFailures ||
      !trace_failures_.emplace(message).second) {
    return;
  }
  std::fprintf(stderr, "[modlock] native effect create failed: %.*s\n",
               static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

void WorldEffectGameFactory::TraceSuccess() {
  if (!InteropTraceEnabled() || trace_success_logged_) {
    return;
  }
  trace_success_logged_ = true;
  std::fprintf(stderr, "[modlock] native effect create succeeded: info_particle_system\n");
  std::fflush(stderr);
}

}  // namespace modlock::render
