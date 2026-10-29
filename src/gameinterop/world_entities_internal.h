#pragma once

// Helpers WorldEntities shares across its files.

#include <cstddef>
#include <cstring>
#include <string_view>

#include "modlock/gameinterop/entity_abi.h"

namespace modlock::gameinterop {

// ReadAt copies a T from offset bytes into instance.
template <typename T>
inline T ReadAt(const void* instance, size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const unsigned char*>(instance) + offset, sizeof(value));
  return value;
}

// ReadLive reads memory of a live entity or of the engine's class tables,
// which the game keeps mapped while WorldEntities runs.
inline bool ReadLive(const void* source, void* out, size_t size) {
  std::memcpy(out, source, size);
  return true;
}

// WriteAt copies value to offset bytes into instance.
template <typename T>
inline void WriteAt(void* instance, size_t offset, const T& value) {
  std::memcpy(static_cast<unsigned char*>(instance) + offset, &value, sizeof(value));
}

// IsStructure reports whether name is a lane structure: a guardian, a walker,
// a base guardian or a base sentry.
inline bool IsStructure(std::string_view name) {
  return name.starts_with("npc_boss_tier") || name == "npc_barrack_boss" ||
         name == "npc_base_defense_sentry";
}

// IsRestoredNpc reports whether name is an NPC that Restore places: a
// structure, a trooper or a neutral.
inline bool IsRestoredNpc(std::string_view name) {
  return IsStructure(name) || name == "npc_trooper" || name == "npc_trooper_neutral" ||
         name == "npc_super_neutral" || name == "npc_neutral_sinners_sacrifice";
}

// IsPickup reports whether name is an item pickup or an idol.
inline bool IsPickup(std::string_view name) {
  return name == "citadel_item_pickup" || name == "citadel_item_pickup_idol";
}

// IsRoundObjective reports whether name is an objective a round leaves in
// the world: a pickup, a cash-in point or a powerup spawner.
inline bool IsRoundObjective(std::string_view name) {
  return IsPickup(name) || name == "citadel_koth_cashin" || name == "citadel_item_powerup_spawner";
}

}  // namespace modlock::gameinterop
