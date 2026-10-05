#include "modlock/gameinterop/world_entities.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/native_damage.h"
#include "modlock/gameinterop/pawn_observer.h"

namespace modlock::gameinterop {
namespace {

template <typename T>
T ReadAt(const void* instance, size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const unsigned char*>(instance) + offset, sizeof(value));
  return value;
}

template <typename T>
void WriteAt(void* instance, size_t offset, const T& value) {
  std::memcpy(static_cast<unsigned char*>(instance) + offset, &value, sizeof(value));
}

bool IsStructure(std::string_view name) {
  return name.starts_with("npc_boss_tier") || name == "npc_barrack_boss" ||
         name == "npc_base_defense_sentry";
}

bool IsRestoredNpc(std::string_view name) {
  return IsStructure(name) || name == "npc_trooper" || name == "npc_trooper_neutral" ||
         name == "npc_super_neutral" || name == "npc_neutral_sinners_sacrifice";
}

// IsSpawnableNpc adds the lane Guardian, whose designer class Restore never
// reconciles, to the restorable NPC classes.
bool IsSpawnableNpc(std::string_view name) {
  return IsRestoredNpc(name) || name == "npc_trooper_boss";
}

bool IsPickup(std::string_view name) {
  return name == "citadel_item_pickup" || name == "citadel_item_pickup_idol";
}

bool IsRoundObjective(std::string_view name) {
  return IsPickup(name) || name == "citadel_koth_cashin" || name == "citadel_item_powerup_spawner";
}

const char* LaneClass(std::string_view name) {
  if (name == "npc_trooper") return "CNPC_Trooper";
  if (name == "npc_boss_tier1") return "CNPC_Boss_Tier1";
  if (name == "npc_boss_tier2") return "CNPC_Boss_Tier2";
  if (name == "npc_boss_tier3") return "CNPC_Boss_Tier3";
  if (name == "npc_barrack_boss") return "CNPC_BarrackBoss";
  return nullptr;
}

std::string DesignerName(void* entity) {
  auto* identity = IdentityOf(entity);
  if (!identity) return {};
  const char* name = ReadAt<const char*>(identity, 0x20);
  if (!name) return {};
  std::string copied;
  for (size_t i = 0; i < 128; ++i) {
    if (!name[i]) return copied;
    if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') ||
          name[i] == '_'))
      return {};
    copied.push_back(name[i]);
  }
  return {};
}

float DistanceSquared(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  float result = 0;
  for (size_t i = 0; i < a.size(); ++i) result += (a[i] - b[i]) * (a[i] - b[i]);
  return result;
}

}  // namespace

std::expected<WorldEntities, std::string> WorldEntities::Resolve(const ModuleImage& server,
                                                                 void* schema_system) {
  WorldEntities world({});
  world.calls_.entity_system = &ResolveLiveEntitySystem;
  world.calls_.schema = schema_system;
  auto key_values = ResolveKeyValuesCalls(server);
  if (!key_values) return std::unexpected(key_values.error());
  world.calls_.key_values = *key_values;
  struct Entry {
    std::string_view id;
    void** slot;
  };
  const Entry entries[] = {
      {"entity-system.create-entity-by-name", reinterpret_cast<void**>(&world.calls_.create)},
      {"entity-system.queue-spawn-entity", reinterpret_cast<void**>(&world.calls_.queue)},
      {"entity-system.execute-queued-creation", reinterpret_cast<void**>(&world.calls_.execute)},
      {"entity.remove", reinterpret_cast<void**>(&world.calls_.remove)},
      {"vdata.lookup-by-hash", reinterpret_cast<void**>(&world.calls_.definition)},
  };
  for (const auto& entry : entries) {
    auto address = ResolveSignature(server, entry.id);
    if (!address) return std::unexpected(address.error());
    *entry.slot = *address;
  }
  struct Field {
    const char* owner;
    const char* name;
    size_t width;
  };
  const Field fields[] = {{"CBaseEntity", "m_iTeamNum", sizeof(uint8_t)},
                          {"CBaseEntity", "m_nSubclassID", sizeof(uint32_t) + sizeof(void*)},
                          {"CBaseEntity", "m_iHealth", sizeof(int32_t)},
                          {"CBaseEntity", "m_iMaxHealth", sizeof(int32_t)},
                          {"CBaseEntity", "m_CBodyComponent", sizeof(void*)},
                          {"CBodyComponent", "m_pSceneNode", sizeof(void*)},
                          {"CGameSceneNode", "m_vecAbsOrigin", sizeof(float) * 3}};
  for (size_t i = 0; i < world.calls_.offsets.size(); ++i) {
    auto field = SchemaFieldOf(schema_system, "server.dll", fields[i].owner, fields[i].name);
    if (!field) return std::unexpected(field.error());
    if (field->size < fields[i].width)
      return std::unexpected(std::string("NPC field storage too short: ") + fields[i].name);
    world.calls_.offsets[i] = field->offset;
  }
  return world;
}

std::expected<WorldEntities::Sample, std::string> WorldEntities::ReadEntity(
    void* entity, std::string name) const {
  const auto handle = ReferenceHandleOf(entity);
  if (!handle) return std::unexpected("NPC identity is no longer live");
  void* body = ReadAt<void*>(entity, calls_.offsets[4]);
  if (!body) return std::unexpected("NPC body is absent");
  void* scene = ReadAt<void*>(body, calls_.offsets[5]);
  if (!scene) return std::unexpected("NPC scene is absent");
  Target state{};
  state.designer_name = std::move(name);
  state.subclass_id = ReadAt<uint32_t>(entity, calls_.offsets[1]);
  state.team = ReadAt<uint8_t>(entity, calls_.offsets[0]);
  state.position = ReadAt<std::array<float, 3>>(scene, calls_.offsets[6]);
  state.health = ReadAt<int32_t>(entity, calls_.offsets[2]);
  state.max_health = ReadAt<int32_t>(entity, calls_.offsets[3]);
  if (const auto* owner = LaneClass(state.designer_name)) {
    auto lane = SchemaFieldOf(calls_.schema, "server.dll", owner, "m_iLane");
    if (!lane || lane->size < sizeof(uint32_t)) return std::unexpected("trooper lane unavailable");
    state.lane = ReadAt<uint32_t>(entity, lane->offset);
  }
  return Sample{*handle, std::move(state)};
}

std::expected<std::vector<WorldEntities::Sample>, std::string> WorldEntities::Read() const {
  auto system = calls_.entity_system();
  if (!system) return std::unexpected(system.error());
  std::vector<Sample> result;
  for (void* entity : EntityInstances(*system)) {
    auto name = DesignerName(entity);
    if (!IsRestoredNpc(name)) continue;
    auto sample = ReadEntity(entity, std::move(name));
    if (!sample) return std::unexpected(sample.error());
    if (sample->state.health > 0) result.push_back(std::move(*sample));
  }
  return result;
}

std::expected<void, std::string> WorldEntities::Apply(void* entity, const Target& target) const {
  TeleportEntity(entity, target.position, target.facing, target.velocity);
  if (target.health <= 0) return {};
  if (!RestorePawnHealth(entity, target.health, target.max_health))
    return std::unexpected("NPC health restoration failed");
  return {};
}

std::expected<void*, std::string> WorldEntities::Create(const Target& target) {
  auto system = calls_.entity_system();
  if (!system) return std::unexpected(system.error());
  void* definition = target.subclass_id ? calls_.definition(-1, target.subclass_id) : nullptr;
  if (target.subclass_id && !definition)
    return std::unexpected("entity subclass is absent from this game build");
  void* entity = calls_.create(nullptr, target.designer_name.c_str(), -1);
  if (!entity) return std::unexpected("native NPC creation failed: " + target.designer_name);
  // Native CBaseEntity::CreateByDesignerName installs this subclass pair before
  // Spawn. The VData pointer follows the four-byte schema token without padding.
  if (target.subclass_id) {
    WriteAt(entity, calls_.offsets[1], target.subclass_id);
    WriteAt(entity, calls_.offsets[1] + sizeof(uint32_t), definition);
  }
  WriteAt(entity, calls_.offsets[0], static_cast<uint8_t>(target.team));
  if (target.lane) {
    const auto* owner = LaneClass(target.designer_name);
    if (!owner) {
      calls_.remove(entity);
      return std::unexpected("NPC lane class is unsupported");
    }
    auto lane = SchemaFieldOf(calls_.schema, "server.dll", owner, "m_iLane");
    if (!lane || lane->size < sizeof(uint32_t)) {
      calls_.remove(entity);
      return std::unexpected("trooper lane unavailable");
    }
    WriteAt(entity, lane->offset, *target.lane);
  }
  TeleportEntity(entity, target.position, target.facing, target.velocity);
  const auto handle = ReferenceHandleOf(entity);
  if (!handle) {
    calls_.remove(entity);
    return std::unexpected("created NPC has no native identity");
  }
  auto key_values = BuildEntityKeyValues(calls_.key_values, {});
  if (!key_values) {
    calls_.remove(entity);
    return std::unexpected("NPC spawn properties: " + key_values.error());
  }
  // Queued creation retains and consumes a native object even without properties.
  calls_.queue(*system, IdentityOf(entity), *key_values);
  calls_.execute(*system);
  entity = EntityInstance(*system, *handle);
  if (!entity) return std::unexpected("NPC did not survive native spawn");
  return entity;
}

std::expected<void, std::string> WorldEntities::Restore(std::span<const Target> targets) {
  pending_.clear();
  // Validate every target and definition before removing any existing world state.
  for (const auto& target : targets) {
    if (!IsRestoredNpc(target.designer_name) || !target.subclass_id || target.health <= 0 ||
        target.max_health <= 0 || target.team < 0 || target.team > 4 ||
        !calls_.definition(-1, target.subclass_id))
      return std::unexpected("unsupported NPC target: " + target.designer_name);
    for (const auto& vector : {target.position, target.facing, target.velocity})
      for (float value : vector)
        if (!std::isfinite(value)) return std::unexpected("NPC target motion is nonfinite");
  }
  auto system = calls_.entity_system();
  if (!system) return std::unexpected(system.error());
  std::vector<std::optional<uint32_t>> retained(targets.size());
  for (void* entity : EntityInstances(*system)) {
    const auto name = DesignerName(entity);
    if (!IsRestoredNpc(name)) continue;
    auto sample = ReadEntity(entity, name);
    if (!sample) return std::unexpected(sample.error());
    bool keep = false;
    if (IsStructure(name) && sample->state.health > 0) {
      for (size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        if (!retained[i] && target.designer_name == name && target.team == sample->state.team &&
            target.subclass_id == sample->state.subclass_id &&
            DistanceSquared(target.position, sample->state.position) < 64 * 64) {
          retained[i] = sample->handle;
          keep = true;
          break;
        }
      }
    }
    if (!keep) calls_.remove(entity);
  }
  for (size_t i = 0; i < targets.size(); ++i) {
    void* entity = retained[i] ? EntityInstance(*system, *retained[i]) : nullptr;
    if (!entity) {
      auto created = Create(targets[i]);
      if (!created) return std::unexpected(created.error());
      entity = *created;
    }
    auto applied = Apply(entity, targets[i]);
    if (!applied) return std::unexpected(applied.error());
    auto handle = ReferenceHandleOf(entity);
    if (!handle) return std::unexpected("restored NPC identity is absent");
    pending_.push_back({*handle, targets[i]});
  }
  return {};
}

std::expected<void, std::string> WorldEntities::FinishRestore() {
  auto system = calls_.entity_system();
  if (!system) return std::unexpected(system.error());
  for (const auto& sample : pending_) {
    void* entity = EntityInstance(*system, sample.handle);
    if (!entity) return std::unexpected("restored NPC did not survive initialization");
    auto applied = Apply(entity, sample.state);
    if (!applied) return std::unexpected(applied.error());
  }
  pending_.clear();
  return {};
}

std::expected<uint32_t, std::string> WorldEntities::CreatePickup(
    Pickup kind, const std::array<float, 3>& position) {
  if (!std::ranges::all_of(position, [](float value) { return std::isfinite(value); }))
    return std::unexpected("pickup position must be finite");
  const bool urn = kind == Pickup::kUrn;
  Target target{};
  target.designer_name = urn ? "citadel_item_pickup_idol" : "citadel_item_pickup";
  // Native pickup Spawn reads its aura from VData. The urn's subclass shares
  // its designer name; movement buffs select the movement pickup definition.
  target.subclass_id =
      MakeMemberName(urn ? "citadel_item_pickup_idol" : "movement_powerup_pickup").hash;
  target.position = position;
  auto entity = Create(target);
  if (!entity) return std::unexpected(entity.error());
  const auto handle = ReferenceHandleOf(*entity);
  if (!handle) return std::unexpected("native pickup identity is absent after spawn");
  return *handle;
}

std::expected<std::optional<WorldEntities::Sample>, std::string> WorldEntities::ReadPickup(
    uint32_t handle) const {
  auto system = calls_.entity_system();
  if (!system) return std::unexpected(system.error());
  void* entity = EntityInstance(*system, handle);
  if (!entity) return std::nullopt;
  auto name = DesignerName(entity);
  if (!IsPickup(name)) return std::unexpected("pickup identity refers to another entity class");
  auto sample = ReadEntity(entity, std::move(name));
  if (!sample) return std::unexpected(sample.error());
  return std::move(*sample);
}

std::expected<void, std::string> WorldEntities::ClearAuthored(const NativeDamage& damage) {
  auto system = calls_.entity_system();
  if (!system) return std::unexpected(system.error());
  std::vector<uint32_t> actors;
  for (void* entity : EntityInstances(*system)) {
    const auto name = DesignerName(entity);
    if (name != "npc_trooper_boss" && name != "npc_boss_tier1" && name != "npc_boss_tier2" &&
        !(IsRestoredNpc(name) && !IsStructure(name)) && !IsRoundObjective(name))
      continue;
    if (const auto handle = ReferenceHandleOf(entity)) actors.push_back(*handle);
  }
  // Native death can remove other actors. Reborrow each serial-fenced identity
  // after every mutation instead of retaining the original instance pointers.
  for (const auto handle : actors) {
    void* entity = EntityInstance(*system, handle);
    if (!entity) continue;
    if (IsRoundObjective(DesignerName(entity))) {
      calls_.remove(entity);
      continue;
    }
    auto killed = damage.Kill(entity);
    if (!killed) return killed;
  }
  return {};
}

std::expected<size_t, std::string> WorldEntities::Remove(std::string_view designer_name) {
  auto system = calls_.entity_system();
  if (!system) return std::unexpected(system.error());
  // UTIL_Remove defers deletion, so the instance snapshot stays valid.
  size_t removed = 0;
  for (void* entity : EntityInstances(*system)) {
    if (DesignerName(entity) != designer_name) continue;
    calls_.remove(entity);
    ++removed;
  }
  return removed;
}

uint32_t WorldEntities::SubclassId(std::string_view vdata_name) {
  return MakeMemberName(vdata_name).hash;
}

std::expected<uint32_t, std::string> WorldEntities::Spawn(const Target& target) {
  if (!IsSpawnableNpc(target.designer_name) || !target.subclass_id || target.team < 0 ||
      target.team > 4 || target.health < 0 || target.max_health < target.health)
    return std::unexpected("unsupported NPC spawn: " + target.designer_name);
  for (const auto& vector : {target.position, target.facing, target.velocity})
    for (float value : vector)
      if (!std::isfinite(value)) return std::unexpected("NPC spawn motion is nonfinite");
  auto created = Create(target);
  if (!created) return std::unexpected(created.error());
  // A spawn that fails after creation is removed, so no untracked NPC remains.
  auto applied = Apply(*created, target);
  if (!applied) {
    calls_.remove(*created);
    return std::unexpected(applied.error());
  }
  const auto handle = ReferenceHandleOf(*created);
  if (!handle) {
    calls_.remove(*created);
    return std::unexpected("spawned NPC identity is absent");
  }
  spawned_.push_back({*handle, target});
  return *handle;
}

void WorldEntities::FinishSpawns() {
  auto system = ResolveLiveEntitySystem();
  if (!system) return;
  for (const auto& sample : spawned_) {
    void* entity = EntityInstance(*system, sample.handle);
    if (!entity) continue;
    // A spawn killed on its first frame stays dead.
    auto live = ReadEntity(entity, DesignerName(entity));
    if (!live || live->state.health <= 0) continue;
    (void)Apply(entity, sample.state);
  }
  spawned_.clear();
}

std::expected<std::optional<WorldEntities::Sample>, std::string> WorldEntities::ReadNpc(
    uint32_t handle) const {
  auto system = ResolveLiveEntitySystem();
  if (!system) return std::unexpected(system.error());
  void* entity = EntityInstance(*system, handle);
  if (!entity) return std::nullopt;
  auto name = DesignerName(entity);
  if (!IsSpawnableNpc(name)) return std::nullopt;
  auto sample = ReadEntity(entity, std::move(name));
  if (!sample) return std::unexpected(sample.error());
  if (sample->state.health <= 0) return std::nullopt;
  return std::move(*sample);
}

std::expected<bool, std::string> WorldEntities::SetHealth(uint32_t handle, int32_t health,
                                                          int32_t max_health) {
  if (health <= 0 || max_health < health) return std::unexpected("NPC health is invalid");
  auto system = ResolveLiveEntitySystem();
  if (!system) return std::unexpected(system.error());
  void* entity = EntityInstance(*system, handle);
  if (!entity || !IsSpawnableNpc(DesignerName(entity))) return false;
  if (!RestorePawnHealth(entity, health, max_health))
    return std::unexpected("NPC health restoration failed");
  return true;
}

std::expected<bool, std::string> WorldEntities::Move(uint32_t handle,
                                                     const std::array<float, 3>& position,
                                                     const std::array<float, 3>& facing,
                                                     const std::array<float, 3>& velocity) {
  auto system = ResolveLiveEntitySystem();
  if (!system) return std::unexpected(system.error());
  void* entity = EntityInstance(*system, handle);
  if (!entity || !IsSpawnableNpc(DesignerName(entity))) return false;
  TeleportEntity(entity, position, facing, velocity);
  return true;
}

std::expected<bool, std::string> WorldEntities::RemoveNpc(uint32_t handle) {
  auto system = ResolveLiveEntitySystem();
  if (!system) return std::unexpected(system.error());
  void* entity = EntityInstance(*system, handle);
  // Only an NPC or pickup this class may create is removed; any other entity
  // is refused.
  if (!entity) return false;
  const auto name = DesignerName(entity);
  if (!IsSpawnableNpc(name) && !IsPickup(name)) return false;
  calls_.remove(entity);
  return true;
}

}  // namespace modlock::gameinterop
