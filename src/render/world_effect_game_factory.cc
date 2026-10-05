#include "modlock/render/world_effect_game_factory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

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
    if (effect_name == shipped) return true;
  }
  return false;
}

struct alignas(8) StringVariant {
  const char* value;
  void* citadel_pad[2]{};
  std::uint8_t type = 30;
  std::uint8_t alignment = 0;
  std::uint16_t flags = 0;
};
static_assert(sizeof(StringVariant) == 32);

// Teleport places an effect entity at origin with angles and no motion.
void Teleport(void* entity, const modlock::Vec3& origin, const std::array<float, 3>& angles) {
  gameinterop::TeleportEntity(entity,
                              {static_cast<float>(origin.x()), static_cast<float>(origin.y()),
                               static_cast<float>(origin.z())},
                              angles, {});
}

}  // namespace

std::expected<std::unique_ptr<WorldEffectGameFactory>, std::string>
WorldEffectGameFactory::TryCreate(const modlock::gameinterop::ModuleImage& server) {
  WorldEffectGameCalls calls;
  struct Entry {
    std::string_view id;
    void** slot;
  };
  const Entry entries[] = {
      {"entity-system.create-entity-by-name",
       reinterpret_cast<void**>(&calls.create_entity_by_name)},
      {"entity-system.queue-spawn-entity", reinterpret_cast<void**>(&calls.queue_spawn_entity)},
      {"entity-system.execute-queued-creation",
       reinterpret_cast<void**>(&calls.execute_queued_creation)},
      {"entity-instance.accept-input", reinterpret_cast<void**>(&calls.accept_input)},
      {"entity.remove", reinterpret_cast<void**>(&calls.util_remove)},
  };
  for (const auto& entry : entries) {
    if (auto address = modlock::gameinterop::ResolveSignature(server, entry.id)) {
      *entry.slot = *address;
    } else {
      return std::unexpected(address.error());
    }
  }
  auto key_values = modlock::gameinterop::ResolveKeyValuesCalls(server);
  if (!key_values) return std::unexpected(key_values.error());
  calls.key_values = *key_values;
  return std::unique_ptr<WorldEffectGameFactory>(new WorldEffectGameFactory(calls));
}

WorldEffectGameFactory::WorldEffectGameFactory(WorldEffectGameCalls calls) : calls_(calls) {}
WorldEffectGameFactory::~WorldEffectGameFactory() = default;

class GameWorldEffect final : public WorldParticle {
 public:
  GameWorldEffect(WorldEffectGameFactory& factory, ParticleSettings settings)
      : factory_(factory), settings_(std::move(settings)) {}
  ~GameWorldEffect() override { Remove(); }

  void Move(const modlock::Vec3& origin,
            const std::optional<std::array<float, 3>>& angles) override {
    if (entity_ == nullptr) return;
    (void)Transform(origin, angles.value_or(settings_.angles));
  }

  std::optional<std::uint32_t> Handle() const override {
    return gameinterop::ReferenceHandleOf(entity_);
  }

  std::expected<void, std::string> Start() override { return Input("Start"); }
  std::expected<void, std::string> Stop() override { return Input("Stop"); }

  std::expected<void, std::string> Transform(const Vec3& origin,
                                             const std::array<float, 3>& angles) override {
    if (!entity_) return std::unexpected("particle is no longer live");
    if (!std::isfinite(origin.x()) || !std::isfinite(origin.y()) || !std::isfinite(origin.z()) ||
        !std::ranges::all_of(angles, [](float v) { return std::isfinite(v); }))
      return std::unexpected("particle transform is not finite");
    Teleport(entity_, origin, angles);
    settings_.origin = origin;
    settings_.angles = angles;
    return {};
  }

  std::expected<void, std::string> Tint(const ParticleTint& tint) override {
    if (!entity_) return std::unexpected("particle is no longer live");
    if (tint.control_point < 0 || tint.control_point >= 64)
      return std::unexpected("particle tint control point must be between 0 and 63");
    auto fields = factory_.ResolveParticleFields();
    if (!fields) return std::unexpected(fields.error());
    if (auto wrote = Write((*fields)->tint, tint.rgba); !wrote) return wrote;
    return Write((*fields)->tint_control_point, tint.control_point);
  }

  std::expected<void, std::string> Data(const ParticleData& data) override {
    if (!entity_) return std::unexpected("particle is no longer live");
    if (data.control_point < 0 || data.control_point >= 64 ||
        !std::ranges::all_of(data.value, [](float v) { return std::isfinite(v); }))
      return std::unexpected("particle data control point or value is invalid");
    auto fields = factory_.ResolveParticleFields();
    if (!fields) return std::unexpected(fields.error());
    if (auto wrote = Write((*fields)->data_control_point, data.control_point); !wrote) return wrote;
    return Write((*fields)->data, data.value);
  }

  std::expected<void, std::string> ControlPoint(int index, std::uint32_t handle) override {
    if (!entity_) return std::unexpected("particle is no longer live");
    auto fields = factory_.ResolveParticleFields();
    if (!fields) return std::unexpected(fields.error());
    auto field = (*fields)->control_points;
    if (index < 0 || static_cast<size_t>(index) >= field.size / sizeof(handle))
      return std::unexpected("particle control point index is outside the native array");
    if (handle != UINT32_MAX && !gameinterop::EntityInstance(factory_.calls_.entity_system, handle))
      return std::unexpected("particle control point entity is no longer live");
    field.offset += static_cast<size_t>(index) * sizeof(handle);
    field.size = sizeof(handle);
    return Write(field, handle);
  }

  std::expected<void, std::string> Attach(std::uint32_t parent_handle) override {
    if (!entity_) return std::unexpected("particle is no longer live");
    void* parent = gameinterop::EntityInstance(factory_.calls_.entity_system, parent_handle);
    if (!parent) return std::unexpected("particle parent is no longer live");
    return Input("SetParent", parent, "!activator");
  }

  std::expected<void, std::string> Detach() override { return Input("ClearParent"); }

  void Remove() override {
    if (entity_ == nullptr) return;
    factory_.calls_.util_remove(entity_);
    entity_ = nullptr;
  }

  void InvalidateAfterEngineReset() override { entity_ = nullptr; }

  std::expected<void, std::string> Spawn() {
    if (factory_.calls_.create_entity_by_name == nullptr)
      return std::unexpected("effect: create-entity-by-name unresolved");
    if (factory_.calls_.entity_system == nullptr || factory_.calls_.queue_spawn_entity == nullptr ||
        factory_.calls_.execute_queued_creation == nullptr ||
        factory_.calls_.accept_input == nullptr || factory_.calls_.util_remove == nullptr)
      return std::unexpected(
          "effect: creation unresolved (entity system, queue, execute, "
          "accept-input, or remove)");

    entity_ = factory_.calls_.create_entity_by_name(nullptr, "info_particle_system", -1);
    if (entity_ == nullptr) return std::unexpected("create info_particle_system returned null");
    const modlock::gameinterop::EntityKeyValue pairs[] = {
        {.key = "effect_name", .value = std::string_view(settings_.resource)},
        {.key = "start_active", .value = settings_.start_active},
    };
    auto built = modlock::gameinterop::BuildEntityKeyValues(factory_.calls_.key_values, pairs);
    if (!built) {
      Remove();
      return std::unexpected("effect: " + built.error());
    }
    if (settings_.tint) {
      if (auto applied = Tint(*settings_.tint); !applied) return applied;
    }
    if (settings_.data) {
      if (auto applied = Data(*settings_.data); !applied) return applied;
    }
    for (const auto& [index, handle] : settings_.control_points) {
      if (auto applied = ControlPoint(index, handle); !applied) return applied;
    }
    if (auto moved = Transform(settings_.origin, settings_.angles); !moved) return moved;
    factory_.calls_.queue_spawn_entity(factory_.calls_.entity_system,
                                       modlock::gameinterop::IdentityOf(entity_), *built);
    factory_.calls_.execute_queued_creation(factory_.calls_.entity_system);
    if (settings_.start_active) {
      if (auto started = Start(); !started) return started;
    }
    if (settings_.parent) {
      if (auto attached = Attach(*settings_.parent); !attached) return attached;
    }
    return {};
  }

 private:
  std::expected<void, std::string> Input(const char* name, void* activator = nullptr,
                                         const char* value = "") {
    if (!entity_) return std::unexpected("particle is no longer live");
    StringVariant variant{.value = value};
    if (!factory_.calls_.accept_input(entity_, name, activator, nullptr, &variant, 0, nullptr))
      return std::unexpected(std::string("info_particle_system rejected ") + name);
    return {};
  }

  template <typename T>
  std::expected<void, std::string> Write(const gameinterop::SchemaField& field, const T& value) {
    if (!entity_) return std::unexpected("particle is no longer live");
    if (field.size < sizeof(T)) return std::unexpected("particle schema field is too small");
    std::memcpy(static_cast<std::byte*>(entity_) + field.offset, &value, sizeof(value));
    if (field.networked && !gameinterop::NotifyEntityStateChanged(entity_))
      return std::unexpected("particle network state notification unavailable");
    return {};
  }

  WorldEffectGameFactory& factory_;
  ParticleSettings settings_;
  void* entity_ = nullptr;
};

// GameWorldEntity owns a visual entity until removal or world invalidation.
class GameWorldEntity final : public WorldEffect {
 public:
  GameWorldEntity(WorldEffectGameFactory& factory, void* entity, std::array<float, 3> angles)
      : factory_(factory), entity_(entity), angles_(angles) {}
  ~GameWorldEntity() override { Remove(); }

  void Move(const Vec3& origin, const std::optional<std::array<float, 3>>& angles) override {
    if (!entity_) return;
    if (angles) angles_ = *angles;
    Teleport(entity_, origin, angles_);
  }

  std::optional<std::uint32_t> Handle() const override {
    return gameinterop::ReferenceHandleOf(entity_);
  }

  void Remove() override {
    if (!entity_) return;
    factory_.calls_.util_remove(entity_);
    entity_ = nullptr;
  }

  void InvalidateAfterEngineReset() override { entity_ = nullptr; }

 private:
  WorldEffectGameFactory& factory_;
  void* entity_;
  std::array<float, 3> angles_;
};

std::expected<std::unique_ptr<WorldEffect>, std::string> WorldEffectGameFactory::CreateEntity(
    const char* class_name, const Vec3& origin, const std::array<float, 3>& angles,
    std::span<const gameinterop::EntityKeyValue> properties) {
  if (!calls_.entity_system || !calls_.create_entity_by_name || !calls_.queue_spawn_entity ||
      !calls_.execute_queued_creation || !calls_.util_remove)
    return std::unexpected("visual entity: native entity creation is unavailable");
  auto values = gameinterop::BuildEntityKeyValues(calls_.key_values, properties);
  if (!values) return std::unexpected(std::string(class_name) + ": " + values.error());
  auto* entity = calls_.create_entity_by_name(nullptr, class_name, -1);
  if (!entity) return std::unexpected(std::string("create ") + class_name + " returned null");
  auto effect = std::make_unique<GameWorldEntity>(*this, entity, angles);
  Teleport(entity, origin, angles);
  calls_.queue_spawn_entity(calls_.entity_system, gameinterop::IdentityOf(entity), *values);
  calls_.execute_queued_creation(calls_.entity_system);
  return effect;
}

std::expected<std::unique_ptr<WorldEffect>, std::string>
WorldEffectGameFactory::CreateFogController(const Vec3& origin, float draw_distance) {
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
    const Vec3& origin, const std::array<float, 3>& angles, const std::array<float, 3>& mins,
    const std::array<float, 3>& maxs, float strength, const std::array<uint8_t, 3>& tint) {
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
  if (!settings.resource.starts_with("models/") || !settings.resource.ends_with(".vmdl") ||
      settings.resource.find("..") != std::string::npos)
    return std::unexpected("model resource must be a game-relative models/*.vmdl path");
  if (!std::isfinite(settings.scale) || settings.scale <= 0)
    return std::unexpected("model scale must be finite and positive");
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
      {.key = "rendercolor", .value = color},
      {.key = "glowcolor", .value = color},
      {.key = "glowstate", .value = settings.glow ? 3 : 0},
      {.key = "glowrange", .value = 0},
      {.key = "glowteam", .value = -1},
      {.key = "disableshadows", .value = true},
      {.key = "use_animgraph", .value = false},
  };
  return CreateEntity("prop_dynamic", settings.origin, settings.angles, properties);
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
  const auto& resource = settings.resource;
  if (!resource.starts_with("particles/") || !resource.ends_with(".vpcf") ||
      resource.find("..") != std::string::npos ||
      !std::ranges::all_of(resource, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '/' || c == '_' || c == '-' || c == '.';
      }))
    return std::unexpected("particle resource must be a game-relative particles/*.vpcf path");
  auto effect = std::make_unique<GameWorldEffect>(*this, settings);
  if (auto spawned = effect->Spawn(); !spawned) {
    TraceFailure(spawned.error());
    return std::unexpected(spawned.error());
  }
  TraceSuccess();
  return effect;
}

std::expected<const ParticleFields*, std::string> WorldEffectGameFactory::ResolveParticleFields() {
  if (particle_fields_) return &*particle_fields_;
  auto schema = gameinterop::ResolveSchemaSystem();
  if (!schema) return std::unexpected(schema.error());
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
    if (!field) return std::unexpected(field.error());
    *destination = *field;
  }
  particle_fields_ = fields;
  return &*particle_fields_;
}

void WorldEffectGameFactory::TraceFailure(std::string_view message) {
  if (!InteropTraceEnabled() || trace_failures_.size() >= kMaxTraceFailures ||
      !trace_failures_.emplace(message).second)
    return;
  std::fprintf(stderr, "[modlock] native effect create failed: %.*s\n",
               static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

void WorldEffectGameFactory::TraceSuccess() {
  if (!InteropTraceEnabled() || trace_success_logged_) return;
  trace_success_logged_ = true;
  std::fprintf(stderr, "[modlock] native effect create succeeded: info_particle_system\n");
  std::fflush(stderr);
}

}  // namespace modlock::render
