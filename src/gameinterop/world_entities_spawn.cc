#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/world_entities.h"
#include "world_entities_internal.h"

namespace modlock::gameinterop {

namespace {

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

}  // namespace

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

std::expected<std::vector<uint32_t>, std::string> WorldEntities::HandlesWhere(
    std::string_view (*read)(void* entity), std::string_view want) {
  // Resolve the entity system.
  auto system = calls_.entity_system();
  if (!system) {
    return std::unexpected(system.error());
  }

  // Collect the handle of every entity with the name.
  std::vector<uint32_t> found;
  for (void* entity : EntityInstances(*system)) {
    if (read(entity) != want) {
      continue;
    }
    if (auto handle = ReferenceHandleOf(entity)) {
      found.push_back(*handle);
    }
  }
  return found;
}

std::expected<std::vector<uint32_t>, std::string> WorldEntities::Find(
    std::string_view designer_name) {
  return HandlesWhere(DesignerName, designer_name);
}

std::expected<std::vector<uint32_t>, std::string> WorldEntities::FindByName(std::string_view name) {
  // Every unnamed entity reads as empty, which names nothing.
  if (name.empty()) {
    return std::vector<uint32_t>{};
  }
  return HandlesWhere(EntityName, name);
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
  void* component = static_cast<unsigned char*>(entity) + calls_.offsets.transmit;
  const auto owned = ReadAt<uint8_t>(component, calls_.offsets.transmit_owned);
  if (owned == 0) {
    WriteAt(component, calls_.offsets.transmit_owned, uint8_t{1});
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
    // Reapply each spawn's state.
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

}  // namespace modlock::gameinterop
