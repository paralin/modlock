#include "modlock/gameinterop/world_entities.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

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

// ReadLive reads memory of a live entity or of the engine's class tables,
// which the game keeps mapped while this class runs.
bool ReadLive(const void* source, void* out, size_t size) {
  std::memcpy(out, source, size);
  return true;
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

bool IsPickup(std::string_view name) {
  return name == "citadel_item_pickup" || name == "citadel_item_pickup_idol";
}

bool IsRoundObjective(std::string_view name) {
  return IsPickup(name) || name == "citadel_koth_cashin" || name == "citadel_item_powerup_spawner";
}

// Unmade names entities the server cannot run when one is created alone: each
// reads state that only a map, a game mode, an owner or the ability that makes
// it provides, and crashes or hangs the server without it. Need says what is
// missing.
struct Unmade {
  std::string_view name;
  std::string_view need;
};

constexpr std::string_view kOwnerHero = "the hero who owns it";
constexpr std::string_view kMakerAbility = "the hero ability that makes it";
constexpr std::string_view kMapData = "game data that only its map provides";

constexpr Unmade kUnmade[] = {
    {"baseplayerpawn", "a player's controller: spawn a hero instead"},
    {"citadel_bounce_pad", kMakerAbility},
    {"citadel_capture_point", "a game mode's capture data"},
    {"citadel_deployable_preview", kMakerAbility},
    {"citadel_herotest_orbspawner", kMapData},
    {"citadel_hideout_prop_base", kOwnerHero},
    {"citadel_hideout_shootable_target_spawner", kMapData},
    {"citadel_magician_turret_object", kMakerAbility},
    {"citadel_mobile_resupply_object", kMakerAbility},
    {"citadel_multi_capture_point", "a game mode's capture data"},
    {"citadel_nano_predatory_statue", kMakerAbility},
    {"citadel_trigger_capture_zipline", "the zipline that owns it"},
    {"env_laser", "a target its map names"},
    {"func_precipitation", "a brush model from its map"},
    {"npc_familiar_helper", kOwnerHero},
    {"npc_neutral_hideout_cat", "the hideout map"},
    {"npc_neutral_hideout_rabbit", "the hideout map"},
    {"npc_player_bot_brain", "a bot player: add a bot instead"},
    {"npc_shielded_sentry", kOwnerHero},
    {"path_node", "a path track from its map"},
    {"path_node_mover", "a path track from its map"},
    {"physics_npc_solver", "an NPC and a physics object to join"},
    {"point_prefab", "a prefab map"},
    {"simple_animating_ai", kMapData},
    {"spark_shower", "a model"},
};

// UnmadeNeed returns what the entity name lacks when created alone, or an
// empty view when the server can create it.
std::string_view UnmadeNeed(std::string_view name) {
  const auto* found = std::ranges::find(kUnmade, name, &Unmade::name);
  return found == std::end(kUnmade) ? std::string_view{} : found->need;
}

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

// DesignerName borrows entity's designer name from the game's string pool,
// which keeps it for the life of the map, or returns empty when the name is
// unreadable. Scans compare it in place, without copying every entity's name.
std::string_view DesignerName(void* entity) {
  // Read the identity's designer name, accepting only a short lowercase identifier.
  auto* identity = IdentityOf(entity);
  if (!identity) {
    return {};
  }
  const char* name = ReadAt<const char*>(identity, 0x20);
  if (!name) {
    return {};
  }
  for (size_t i = 0; i < 128; ++i) {
    if (!name[i]) {
      return {name, i};
    }
    if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') ||
          name[i] == '_')) {
      return {};
    }
  }
  return {};
}

float DistanceSquared(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  float result = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    result += (a[i] - b[i]) * (a[i] - b[i]);
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

  // Resolve each native call by its signature.
  struct Entry {
    std::string_view id;
    void** slot;
  };
  const Entry entries[] = {
      {"entity-system.find-class", reinterpret_cast<void**>(&world.calls_.find_class)},
      {"entity-system.create-entity-by-name", reinterpret_cast<void**>(&world.calls_.create)},
      {"entity-system.queue-spawn-entity", reinterpret_cast<void**>(&world.calls_.queue)},
      {"entity-system.execute-queued-creation", reinterpret_cast<void**>(&world.calls_.execute)},
      {"entity.remove", reinterpret_cast<void**>(&world.calls_.remove)},
      {"vdata.lookup-by-hash", reinterpret_cast<void**>(&world.calls_.definition)},
      {"entity-instance.accept-input", reinterpret_cast<void**>(&world.calls_.accept_input)},
      {"network-transmit.set-state", reinterpret_cast<void**>(&world.calls_.set_transmit_state)},
  };
  for (const auto& entry : entries) {
    auto address = ResolveSignature(server, entry.id);
    if (!address) {
      return std::unexpected(address.error());
    }
    *entry.slot = *address;
  }

  // Resolve each field the NPCs read and write, with its minimum width.
  struct Field {
    const char* owner;
    const char* name;
    size_t width;
  };
  const Field fields[] = {
      {"CBaseEntity", "m_iTeamNum", sizeof(uint8_t)},
      {"CBaseEntity", "m_nSubclassID", sizeof(uint32_t) + sizeof(void*)},
      {"CBaseEntity", "m_iHealth", sizeof(int32_t)},
      {"CBaseEntity", "m_iMaxHealth", sizeof(int32_t)},
      {"CBaseEntity", "m_CBodyComponent", sizeof(void*)},
      {"CBodyComponent", "m_pSceneNode", sizeof(void*)},
      {"CGameSceneNode", "m_vecAbsOrigin", sizeof(float) * 3},
      {"CBaseEntity", "m_NetworkTransmitComponent", sizeof(uint8_t)},
      {"CNetworkTransmitComponent", "m_nTransmitStateOwnedCounter", sizeof(uint8_t)}};
  for (size_t i = 0; i < world.calls_.offsets.size(); ++i) {
    auto field = SchemaFieldOf(schema_system, "server.dll", fields[i].owner, fields[i].name);
    if (!field) {
      return std::unexpected(field.error());
    }
    if (field->size < fields[i].width) {
      return std::unexpected(std::string("NPC field storage too short: ") + fields[i].name);
    }
    world.calls_.offsets[i] = field->offset;
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
  void* body = ReadAt<void*>(entity, calls_.offsets[4]);
  if (!body) {
    return std::unexpected("NPC body is absent");
  }
  void* scene = ReadAt<void*>(body, calls_.offsets[5]);
  if (!scene) {
    return std::unexpected("NPC scene is absent");
  }

  // Read its class, team and position.
  Target state{};
  state.designer_name = std::move(name);
  state.subclass_id = ReadAt<uint32_t>(entity, calls_.offsets[1]);
  state.team = ReadAt<uint8_t>(entity, calls_.offsets[0]);
  state.position = ReadAt<std::array<float, 3>>(scene, calls_.offsets[6]);

  // Read its health and lane.
  state.health = ReadAt<int32_t>(entity, calls_.offsets[2]);
  state.max_health = ReadAt<int32_t>(entity, calls_.offsets[3]);
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
  void* definition = target.subclass_id ? calls_.definition(-1, target.subclass_id) : nullptr;
  if (target.subclass_id && !definition) {
    return std::unexpected("entity subclass is absent from this game build");
  }

  // Create the entity.
  void* entity = calls_.create(nullptr, target.designer_name.c_str(), -1);
  if (!entity) {
    return std::unexpected("the server cannot create " + target.designer_name);
  }

  // Native CBaseEntity::CreateByDesignerName installs this subclass pair before
  // Spawn. The VData pointer follows the four-byte schema token without padding.
  if (target.subclass_id) {
    WriteAt(entity, calls_.offsets[1], target.subclass_id);
    WriteAt(entity, calls_.offsets[1] + sizeof(uint32_t), definition);
  }

  // Set its team, its lane and the caller's preparation.
  WriteAt(entity, calls_.offsets[0], static_cast<uint8_t>(target.team));
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

std::expected<void, std::string> WorldEntities::Restore(std::span<const Target> targets) {
  // Forget the last restore.
  pending_.clear();

  // Validate every target and definition before removing any existing world state.
  for (const auto& target : targets) {
    if (!IsRestoredNpc(target.designer_name) || !target.subclass_id || target.health <= 0 ||
        target.max_health <= 0 || target.team < 0 || target.team > 4 ||
        !calls_.definition(-1, target.subclass_id)) {
      return std::unexpected("unsupported NPC target: " + target.designer_name);
    }
    for (const auto& vector : {target.position, target.facing, target.velocity}) {
      for (float value : vector) {
        if (!std::isfinite(value)) {
          return std::unexpected("NPC target motion is nonfinite");
        }
      }
    }
  }

  // Remove every restored NPC but a living structure that matches a target.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  std::vector<std::optional<uint32_t>> retained(targets.size());
  for (void* entity : EntityInstances(*system)) {
    const auto name = DesignerName(entity);
    if (!IsRestoredNpc(name)) {
      continue;
    }
    auto sample = ReadEntity(entity, std::string(name));
    if (!sample) {
      return std::unexpected(sample.error());
    }
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
    if (!keep) {
      calls_.remove(entity);
    }
  }

  // Create the missing targets and place every one.
  for (size_t i = 0; i < targets.size(); ++i) {
    void* entity = retained[i] ? EntityInstance(*system, *retained[i]) : nullptr;
    if (!entity) {
      auto created = Create(targets[i]);
      if (!created) {
        return std::unexpected(created.error());
      }
      entity = *created;
    }
    auto applied = Apply(entity, targets[i]);
    if (!applied) {
      return std::unexpected(applied.error());
    }
    auto handle = ReferenceHandleOf(entity);
    if (!handle) {
      return std::unexpected("restored NPC identity is absent");
    }
    pending_.push_back({*handle, targets[i]});
  }
  return {};
}

std::expected<void, std::string> WorldEntities::FinishRestore() {
  // Place each pending NPC again after its initialization.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  for (const auto& sample : pending_) {
    void* entity = EntityInstance(*system, sample.handle);
    if (!entity) {
      return std::unexpected("restored NPC did not survive initialization");
    }
    auto applied = Apply(entity, sample.state);
    if (!applied) {
      return std::unexpected(applied.error());
    }
  }
  pending_.clear();
  return {};
}

std::expected<void, std::string> WorldEntities::ClearAuthored(const NativeDamage& damage) {
  // Collect the authored actors.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  std::vector<uint32_t> actors;
  for (void* entity : EntityInstances(*system)) {
    const auto name = DesignerName(entity);
    if (name != "npc_trooper_boss" && name != "npc_boss_tier2" &&
        !(IsRestoredNpc(name) && !IsStructure(name)) && !IsRoundObjective(name)) {
      continue;
    }
    if (const auto handle = ReferenceHandleOf(entity)) {
      actors.push_back(*handle);
    }
  }

  // Native death can remove other actors. Reborrow each serial-fenced identity
  // after every mutation instead of retaining the original instance pointers.
  for (const auto handle : actors) {
    void* entity = EntityInstance(*system, handle);
    if (!entity) {
      continue;
    }
    if (IsRoundObjective(DesignerName(entity))) {
      calls_.remove(entity);
      continue;
    }
    auto killed = damage.Kill(entity);
    if (!killed) {
      return killed;
    }
  }
  return {};
}

std::expected<size_t, std::string> WorldEntities::Remove(std::string_view designer_name) {
  // Resolve the entity system.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }

  // UTIL_Remove defers deletion, so the instance snapshot stays valid.
  size_t removed = 0;
  for (void* entity : EntityInstances(*system)) {
    if (DesignerName(entity) != designer_name) {
      continue;
    }
    calls_.remove(entity);
    ++removed;
  }
  return removed;
}

std::expected<std::vector<uint32_t>, std::string> WorldEntities::Find(
    std::string_view designer_name) {
  // Collect the handle of every entity with the name.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  std::vector<uint32_t> found;
  for (void* entity : EntityInstances(*system)) {
    if (DesignerName(entity) != designer_name) {
      continue;
    }
    if (auto handle = ReferenceHandleOf(entity)) {
      found.push_back(*handle);
    }
  }
  return found;
}

uint32_t WorldEntities::SubclassId(std::string_view vdata_name) {
  return MakeMemberName(vdata_name).hash;
}

std::expected<uint32_t, std::string> WorldEntities::Spawn(const Target& target,
                                                          const Prepare& prepare) {
  // Create the NPC with valid health.
  if (target.health < 0 || target.max_health < target.health) {
    return std::unexpected("NPC health is invalid");
  }
  auto handle = CreateEntity(target, {}, prepare);
  if (!handle) {
    return std::unexpected(handle.error());
  }

  // A spawn that fails after creation is removed, so no untracked NPC remains.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  void* entity = EntityInstance(*system, *handle);
  if (!entity) {
    return std::unexpected(target.designer_name + " did not survive its spawn");
  }
  if (auto applied = Apply(entity, target); !applied) {
    calls_.remove(entity);
    return std::unexpected(applied.error());
  }
  spawned_.push_back({*handle, target});
  return *handle;
}

std::expected<uint32_t, std::string> WorldEntities::CreateEntity(
    const Target& target, std::span<const EntityKeyValue> key_values, const Prepare& prepare) {
  // Refuse an unnamed or misplaced entity.
  if (target.designer_name.empty()) {
    return std::unexpected("an entity needs a designer name");
  }
  for (const auto& vector : {target.position, target.facing}) {
    for (float value : vector) {
      if (!std::isfinite(value)) {
        return std::unexpected("entity placement is nonfinite");
      }
    }
  }

  // A networked string past the encoder's limit stops the server at the next
  // network pack, so a long text value is refused before the entity exists.
  for (const auto& pair : key_values) {
    const auto* text = std::get_if<std::string_view>(&pair.value);
    if (text && text->size() > kMaxKeyValueText) {
      return std::unexpected("key value " + std::string(pair.key) + " is " +
                             std::to_string(text->size()) + " characters; at most " +
                             std::to_string(kMaxKeyValueText) + " can be networked");
    }
  }

  // Some entities crash the server when created alone; they are refused
  // before they exist.
  if (const auto need = UnmadeNeed(target.designer_name); !need.empty()) {
    return std::unexpected(target.designer_name +
                           " crashes the server when created alone: it needs " + std::string(need));
  }

  // A hero holds its abilities, items and weapons. One created alone crashes
  // the server once it is removed or thinks, so it is refused before it
  // exists.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }
  void* entity_class = calls_.find_class(*system, target.designer_name.c_str(), nullptr);
  if (!entity_class) {
    return std::unexpected("the server has no entity " + target.designer_name);
  }
  auto schema = SchemaClassOfEntityClass(entity_class, ReadLive);
  if (!schema) {
    return std::unexpected(schema.error());
  }
  if (SchemaClassDerivesFrom(*schema, "CCitadelBaseAbility")) {
    return std::unexpected(target.designer_name +
                           " is an ability, which only a hero holds: give it with GiveItem or "
                           "ReplaceAbility");
  }

  // Create it and read its handle.
  auto created = Create(target, key_values, prepare);
  if (!created) {
    return std::unexpected(created.error());
  }
  const auto handle = ReferenceHandleOf(*created);
  if (!handle) {
    calls_.remove(*created);
    return std::unexpected("created entity identity is absent");
  }
  return *handle;
}

std::expected<bool, std::string> WorldEntities::FireInput(uint32_t handle, const std::string& input,
                                                          const std::optional<EntityValue>& value,
                                                          std::optional<uint32_t> activator) {
  // Find the entity and its activator.
  auto system = ResolveLiveEntitySystem();
  if (!system) {
    return std::unexpected(system.error());
  }
  void* entity = EntityInstance(*system, handle);
  if (!entity) {
    return false;
  }
  void* cause = nullptr;
  if (activator) {
    cause = EntityInstance(*system, *activator);
    if (!cause) {
      return std::unexpected("the input's activator is gone");
    }
  }

  // The variant borrows a vector from value and text from a terminated copy.
  Variant variant;
  std::string text;
  if (value) {
    std::visit(
        [&]<typename T>(const T& held) {
          if constexpr (std::is_same_v<T, bool>) {
            variant.boolean = held;
            variant.type = VariantType::kBoolean;
          } else if constexpr (std::is_same_v<T, int>) {
            variant.int32 = held;
            variant.type = VariantType::kInt32;
          } else if constexpr (std::is_same_v<T, float>) {
            variant.float32 = held;
            variant.type = VariantType::kFloat32;
          } else if constexpr (std::is_same_v<T, std::string_view>) {
            text = held;
            variant.pointer = text.c_str();
            variant.type = VariantType::kCString;
          } else if constexpr (std::is_same_v<T, KeyValueColor>) {
            variant.color = held;
            variant.type = VariantType::kColor32;
          } else {
            variant.pointer = &held;
            variant.type = VariantType::kVector;
          }
        },
        *value);
  }
  if (!calls_.accept_input(entity, input.c_str(), cause, nullptr, &variant, 0, nullptr)) {
    return std::unexpected(std::string(DesignerName(entity)) + " has no input " + input);
  }
  return true;
}

std::expected<bool, std::string> WorldEntities::TransmitAlways(uint32_t handle) {
  // kTransmitAlways is the transmit state that sends an entity to every
  // client.
  constexpr uint32_t kTransmitAlways = 4;
  auto system = ResolveLiveEntitySystem();
  if (!system) {
    return std::unexpected(system.error());
  }
  void* entity = EntityInstance(*system, handle);
  if (!entity) {
    return false;
  }

  // While its owned counter is nonzero, the game keeps the state set here
  // instead of asking the entity again.
  void* component = static_cast<unsigned char*>(entity) + calls_.offsets[7];
  const auto owned = ReadAt<uint8_t>(component, calls_.offsets[8]);
  if (owned == 0) {
    WriteAt(component, calls_.offsets[8], uint8_t{1});
  }
  calls_.set_transmit_state(component, kTransmitAlways);
  return true;
}

std::expected<bool, std::string> WorldEntities::RemoveEntity(uint32_t handle) {
  // Remove the entity if it still exists.
  auto system = ResolveLiveEntitySystem();
  if (!system) {
    return std::unexpected(system.error());
  }
  void* entity = EntityInstance(*system, handle);
  if (!entity) {
    return false;
  }
  calls_.remove(entity);
  return true;
}

void WorldEntities::FinishSpawns() {
  auto system = ResolveLiveEntitySystem();
  if (!system) {
    return;
  }
  for (const auto& sample : spawned_) {
    void* entity = EntityInstance(*system, sample.handle);
    if (!entity) {
      continue;
    }
    // A spawn killed on its first frame stays dead.
    auto live = ReadEntity(entity, std::string(DesignerName(entity)));
    if (!live || live->state.health <= 0) {
      continue;
    }
    (void)Apply(entity, sample.state);
  }
  spawned_.clear();
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
