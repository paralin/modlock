#include "modlock/gameinterop/world_entities.h"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <vector>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "world_entities_internal.h"

namespace modlock::gameinterop {

namespace {

// kWorldTextPanel is the stock text panel the client draws in the world.
constexpr std::string_view kWorldTextPanel = "point_clientui_world_text_panel";

// kWorldTextPanelDefaults are the layout and geometry the client's own spawner
// gives a text panel. A panel spawned without them crashes the client.
constexpr EntityKeyValue kWorldTextPanelDefaults[] = {
    {.key = "enabled", .value = true},
    {.key = "width", .value = 128.0f},
    {.key = "height", .value = 32.0f},
    {.key = "panel_dpi", .value = 1.0f},
    {.key = "interact_distance", .value = 0.0f},
    {.key = "ignore_input", .value = true},
    {.key = "dialog_layout_name",
     .value =
         std::string_view("file://{resources}/layout/hideout/hideout_in_world_text_label.xml")},
    {.key = "owner_context", .value = 0},
    {.key = "horizontal_align", .value = 1},
    {.key = "vertical_align", .value = 0},
    {.key = "orientation", .value = 1},
    {.key = "ignore_parent_orientation", .value = true},
};

// Subclassed names entities that read a game data entry while spawning and
// fault on a null one, where no entry carries the class's own name. Example
// is one entry each works with. Classes whose entry has their own name, such
// as citadel_hideout_ball, get it by default instead.
struct Subclassed {
  std::string_view name;
  std::string_view example;
};

constexpr Subclassed kSubclassed[] = {
    {"citadel_breakable_prop", "citadel_breakable_prop_vase"},
    {"citadel_capture_point_escort", "capture_point_escort"},
    {"citadel_chesspiece_entity", "citadel_chesspiece_bishop"},
    {"citadel_item_powerup_spawner", "citadel_item_powerup_spawner_bounty_runes"},
    {"citadel_pickup_spawner", "citadel_pickup_floating_apple"},
    {"item_xp", "xp_orb"},
    {"npc_escort", "shield_escort"},
    {"npc_neutral_sinners_sacrifice", "neutral_sinners_sacrifice"},
    {"npc_trooper", "trooper_normal"},
    {"npc_trooper_boss", "npc_boss_tier1"},
    {"npc_trooper_neutral", "neutral_trooper_weak"},
};

// WithWorldTextPanelDefaults returns values followed by each stock default
// whose key values does not set.
std::vector<EntityKeyValue> WithWorldTextPanelDefaults(std::span<const EntityKeyValue> values) {
  std::vector<EntityKeyValue> result(values.begin(), values.end());
  for (const auto& fallback : kWorldTextPanelDefaults) {
    if (std::ranges::find(values, fallback.key, &EntityKeyValue::key) == values.end()) {
      result.push_back(fallback);
    }
  }
  return result;
}

}  // namespace

std::expected<WorldEntities, std::string> WorldEntities::Resolve(const ModuleImage& server,
                                                                 void* schema_system) {
  // Start from the live entity system and the schema.
  WorldEntities world({});
  world.calls_.entity_system = &ResolveLiveEntitySystem;
  world.calls_.schema = schema_system;

  // Resolve the key values calls.
  auto key_values = ResolveKeyValuesCalls(server);
  if (!key_values) {
    return std::unexpected(key_values.error());
  }
  world.calls_.key_values = *key_values;

  // Resolve each native call by its signature, or fail at the first missing one.
  const SignatureCall signatures[] = {
      {"entity-system.find-class", &world.calls_.find_class},
      {"entity-system.create-entity-by-name", &world.calls_.create},
      {"entity-system.queue-spawn-entity", &world.calls_.queue},
      {"entity-system.execute-queued-creation", &world.calls_.execute},
      {"entity.remove", &world.calls_.remove},
      {"vdata.lookup-by-hash", &world.calls_.definition},
      {"entity-instance.accept-input", &world.calls_.accept_input},
      {"network-transmit.set-state", &world.calls_.set_transmit_state},
  };
  auto resolved = ResolveSignatures(server, signatures);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  // Resolve each field the NPCs read and write, with its minimum width.
  auto& offsets = world.calls_.offsets;
  const SchemaOffset fields[] = {
      {"CBaseEntity", "m_iTeamNum", sizeof(uint8_t), &offsets.team},
      {"CBaseEntity", "m_nSubclassID", sizeof(uint32_t) + sizeof(void*), &offsets.subclass},
      {"CBaseEntity", "m_iHealth", sizeof(int32_t), &offsets.health},
      {"CBaseEntity", "m_iMaxHealth", sizeof(int32_t), &offsets.max_health},
      {"CBaseEntity", "m_CBodyComponent", sizeof(void*), &offsets.body},
      {"CBodyComponent", "m_pSceneNode", sizeof(void*), &offsets.scene_node},
      {"CGameSceneNode", "m_vecAbsOrigin", sizeof(float) * 3, &offsets.origin},
      {"CBaseEntity", "m_NetworkTransmitComponent", sizeof(uint8_t), &offsets.transmit},
      {"CNetworkTransmitComponent", "m_nTransmitStateOwnedCounter", sizeof(uint8_t),
       &offsets.transmit_owned},
  };
  if (auto resolved = ResolveSchemaOffsets(schema_system, "server.dll", fields); !resolved) {
    return std::unexpected(resolved.error());
  }
  return world;
}

std::expected<WorldEntities::Sample, std::string> WorldEntities::ReadEntity(
    void* entity, std::string name) const {
  // Read the entity's identity, body and scene.
  const auto handle = ReferenceHandleOf(entity);
  if (!handle) {
    return std::unexpected("NPC identity is no longer live");
  }
  void* body = ReadAt<void*>(entity, calls_.offsets.body);
  if (!body) {
    return std::unexpected("NPC body is absent");
  }
  void* scene = ReadAt<void*>(body, calls_.offsets.scene_node);
  if (!scene) {
    return std::unexpected("NPC scene is absent");
  }

  // Read its class, team and position.
  Target state{};
  state.designer_name = std::move(name);
  state.subclass_id = ReadAt<uint32_t>(entity, calls_.offsets.subclass);
  state.team = ReadAt<uint8_t>(entity, calls_.offsets.team);
  state.position = ReadAt<std::array<float, 3>>(scene, calls_.offsets.origin);

  // Read its health and lane.
  state.health = ReadAt<int32_t>(entity, calls_.offsets.health);
  state.max_health = ReadAt<int32_t>(entity, calls_.offsets.max_health);
  if (const auto lane = LaneOffset(entity)) {
    state.lane = ReadAt<uint32_t>(entity, *lane);
  }
  return Sample{*handle, std::move(state)};
}

std::optional<size_t> WorldEntities::LaneOffset(void* entity) const {
  // Read the entity's class; each class's lane field resolves once.
  auto schema = SchemaClassOfEntity(entity, ReadLive);
  if (!schema) {
    return std::nullopt;
  }
  if (auto found = lanes_.find(*schema); found != lanes_.end()) {
    return found->second;
  }

  // Resolve the class's lane field and remember it.
  const std::string name(SchemaClassNameOf(*schema));
  auto lane = SchemaFieldOf(calls_.schema, "server.dll", name.c_str(), "m_iLane");
  std::optional<size_t> offset;
  if (lane && lane->size >= sizeof(uint32_t)) {
    offset = lane->offset;
  }
  lanes_.emplace(*schema, offset);
  return offset;
}

std::expected<std::vector<WorldEntities::Sample>, std::string> WorldEntities::Read() const {
  // Sample every living restored NPC.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  std::vector<Sample> result;
  for (void* entity : EntityInstances(*system)) {
    // Collect each live restored NPC.
    const auto name = DesignerName(entity);
    if (!IsRestoredNpc(name)) {
      continue;
    }
    auto sample = ReadEntity(entity, std::string(name));
    if (!sample) {
      return std::unexpected(sample.error());
    }
    if (sample->state.health > 0) {
      result.push_back(std::move(*sample));
    }
  }
  return result;
}

std::expected<void, std::string> WorldEntities::Apply(void* entity, const Target& target) const {
  TeleportEntity(entity, target.position, target.facing, target.velocity);
  if (target.health <= 0) {
    return {};
  }
  if (!RestorePawnHealth(entity, target.health, target.max_health)) {
    return std::unexpected("NPC health restoration failed");
  }
  return {};
}

std::expected<void*, std::string> WorldEntities::Create(const Target& target,
                                                        std::span<const EntityKeyValue> key_values,
                                                        const Prepare& prepare) {
  // Resolve the entity system and the subclass definition.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  // Without a subclass, use the game data entry named for the class when one
  // exists: classes such as citadel_hideout_ball read theirs while spawning
  // and fault on a null entry.
  uint32_t subclass_id = target.subclass_id ? target.subclass_id : SubclassId(target.designer_name);
  void* definition = calls_.definition(-1, subclass_id);
  if (!definition) {
    if (target.subclass_id) {
      return std::unexpected("entity subclass is absent from this game build");
    }
    const auto* subclassed =
        std::ranges::find(kSubclassed, target.designer_name, &Subclassed::name);
    if (subclassed != std::end(kSubclassed)) {
      return std::unexpected(target.designer_name +
                             " crashes the server without a subclass, such as " +
                             std::string(subclassed->example));
    }
    subclass_id = 0;
  }

  // Create the entity.
  void* entity = calls_.create(nullptr, target.designer_name.c_str(), -1);
  if (!entity) {
    return std::unexpected("the server cannot create " + target.designer_name);
  }

  // Native CBaseEntity::CreateByDesignerName installs this subclass pair before
  // Spawn. The VData pointer follows the four-byte schema token without padding.
  if (subclass_id) {
    WriteAt(entity, calls_.offsets.subclass, subclass_id);
    WriteAt(entity, calls_.offsets.subclass + sizeof(uint32_t), definition);
  }

  // Set its team, its lane and the caller's preparation.
  WriteAt(entity, calls_.offsets.team, static_cast<uint8_t>(target.team));
  if (target.lane) {
    const auto lane = LaneOffset(entity);
    if (!lane) {
      calls_.remove(entity);
      return std::unexpected(target.designer_name + " walks no lane");
    }
    WriteAt(entity, *lane, *target.lane);
  }
  if (prepare) {
    if (auto prepared = prepare(entity); !prepared) {
      calls_.remove(entity);
      return std::unexpected(prepared.error());
    }
  }

  // Place it and read its identity.
  TeleportEntity(entity, target.position, target.facing, target.velocity);
  const auto handle = ReferenceHandleOf(entity);
  if (!handle) {
    calls_.remove(entity);
    return std::unexpected(target.designer_name + " has no native identity");
  }

  // Build its key values, with a world text panel's defaults.
  std::vector<EntityKeyValue> panel_values;
  if (target.designer_name == kWorldTextPanel) {
    panel_values = WithWorldTextPanelDefaults(key_values);
    key_values = panel_values;
  }
  auto built = BuildEntityKeyValues(calls_.key_values, key_values);
  if (!built) {
    calls_.remove(entity);
    return std::unexpected(target.designer_name + " key values: " + built.error());
  }

  // Queued creation retains and consumes a native object even without properties.
  calls_.queue(*system, IdentityOf(entity), *built);
  calls_.execute(*system);
  entity = EntityInstance(*system, *handle);
  if (!entity) {
    return std::unexpected(target.designer_name + " did not survive its spawn");
  }
  return entity;
}

std::expected<bool, std::string> WorldEntities::Exists(uint32_t handle) const {
  auto system = ResolveLiveEntitySystem();
  if (!system) {
    return std::unexpected(system.error());
  }
  return EntityInstance(*system, handle) != nullptr;
}

std::expected<std::optional<WorldEntities::Sample>, std::string> WorldEntities::ReadNpc(
    uint32_t handle) const {
  // Sample the NPC if it still lives.
  auto system = ResolveLiveEntitySystem();
  if (!system) {
    return std::unexpected(system.error());
  }
  void* entity = EntityInstance(*system, handle);
  if (!entity) {
    return std::nullopt;
  }
  auto sample = ReadEntity(entity, std::string(DesignerName(entity)));
  if (!sample) {
    return std::unexpected(sample.error());
  }
  if (sample->state.health <= 0) {
    return std::nullopt;
  }
  return std::move(*sample);
}

std::expected<bool, std::string> WorldEntities::SetHealth(uint32_t handle, int32_t health,
                                                          int32_t max_health) {
  // Set the health of a live NPC.
  if (health <= 0 || max_health < health) {
    return std::unexpected("NPC health is invalid");
  }
  auto system = ResolveLiveEntitySystem();
  if (!system) {
    return std::unexpected(system.error());
  }
  void* entity = EntityInstance(*system, handle);
  if (!entity) {
    return false;
  }
  if (!RestorePawnHealth(entity, health, max_health)) {
    return std::unexpected("NPC health restoration failed");
  }
  return true;
}

std::expected<bool, std::string> WorldEntities::Move(uint32_t handle,
                                                     const std::array<float, 3>& position,
                                                     const std::array<float, 3>& facing,
                                                     const std::array<float, 3>& velocity) {
  // Teleport the entity if it still exists.
  auto system = ResolveLiveEntitySystem();
  if (!system) {
    return std::unexpected(system.error());
  }
  void* entity = EntityInstance(*system, handle);
  if (!entity) {
    return false;
  }
  TeleportEntity(entity, position, facing, velocity);
  return true;
}

}  // namespace modlock::gameinterop
