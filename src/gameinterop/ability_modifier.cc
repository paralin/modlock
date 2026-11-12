#include "modlock/gameinterop/ability_modifier.h"

#include <algorithm>
#include <cstring>

#include "modlock/gameinterop/keyvalues.h"
#include "utl_vector.h"

namespace modlock::gameinterop {

std::expected<AbilityModifier, std::string> AbilityModifier::Resolve(const ModuleImage& server,
                                                                     void* schema,
                                                                     std::string_view key,
                                                                     float duration) {
  // Deadworks NativeAbility and CModifierProperty provide the native call
  // contract.
  auto lookup = ResolveSignature(server, "vdata.lookup-by-hash");
  auto add = ResolveSignature(server, "modifier.add");
  if (!lookup) {
    return std::unexpected(lookup.error());
  }
  if (!add) {
    return std::unexpected(add.error());
  }
  return Resolve(reinterpret_cast<AbilityDefinitions::Lookup>(*lookup),
                 reinterpret_cast<AddModifier>(*add), schema, key, duration);
}

std::expected<AbilityModifier, std::string> AbilityModifier::Resolve(
    AbilityDefinitions::Lookup lookup, AddModifier add, void* schema, std::string_view key,
    float duration) {
  if (!lookup || !add) {
    return std::unexpected("modifier calls unavailable");
  }

  // Find the modifier property and life-state layout.
  const auto property = SchemaFieldOf(schema, "server.dll", "CBaseEntity", "m_pModifierProp");
  const auto life_state = SchemaFieldOf(schema, "server.dll", "CBaseEntity", "m_lifeState");
  const auto modifiers = SchemaFieldOf(schema, "server.dll", "CModifierProperty", "m_vecModifiers");
  const auto predicted =
      SchemaFieldOf(schema, "server.dll", "CModifierProperty", "m_bPredictedOwner");
  const auto lifetime = SchemaFieldOf(schema, "server.dll", "CModifierVData", "m_flDuration");
  if (!property || !life_state || !modifiers || !predicted || !lifetime) {
    return std::unexpected("modifier property schema unavailable");
  }

  // Find the modifier's definition.
  auto* definition =
      static_cast<unsigned char*>(lookup(AbilityDefinitions::kModifiers, MakeMemberName(key).hash));
  if (!definition) {
    return std::unexpected("modifier definition " + std::string(key) + " unavailable");
  }

  // Swap in the duration, keeping the original to restore.
  AbilityModifier modifier;
  std::memcpy(&modifier.previous_, definition + lifetime->offset, sizeof(modifier.previous_));
  std::memcpy(definition + lifetime->offset, &duration, sizeof(duration));

  // Record the call, parent ability subclass (when embedded) and offsets.
  modifier.add_ = add;
  modifier.definition_ = definition;
  if (const auto slash = key.find('/'); slash != std::string_view::npos && slash > 0) {
    modifier.ability_subclass_ = MakeMemberName(key.substr(0, slash)).hash;
  }
  modifier.duration_ = lifetime->offset;
  modifier.property_ = property->offset;
  modifier.modifiers_ = modifiers->offset;
  modifier.predicted_ = predicted->offset;
  modifier.life_state_ = life_state->offset;
  return modifier;
}

std::expected<void, std::string> AbilityModifier::Attach(PawnObserver& observer, int32_t slot,
                                                         uint32_t ability) const {
  // The pawn must own the source ability.
  auto property = Property(observer, slot);
  if (!property) {
    return std::unexpected(property.error());
  }
  const auto sample = observer.Observe(slot);
  auto owned = observer.CurrentAbilitiesForSlot(slot);
  if (!sample || !owned) {
    return std::unexpected("modifier pawn unavailable");
  }
  const auto source = std::ranges::find(*owned, ability, &PawnObserver::Ability::subclass_id);
  if (source == owned->end()) {
    return std::unexpected("modifier source ability unavailable");
  }

  // The native call replicates only from a predicted owner.
  auto* pawn = observer.PawnForSlot(slot);
  const auto predicted = (*property)[predicted_];
  (*property)[predicted_] = 1;
  void* attached = add_(*property, pawn, source->handle, static_cast<uint32_t>(sample->team),
                        definition_, nullptr, nullptr);
  (*property)[predicted_] = predicted;
  if (!attached) {
    return std::unexpected("modifier was not attached");
  }
  return {};
}

std::expected<bool, std::string> AbilityModifier::Attached(PawnObserver& observer,
                                                           int32_t slot) const {
  auto property = Property(observer, slot);
  if (!property) {
    return std::unexpected(property.error());
  }
  return HasDefinition(*property);
}

std::expected<void, std::string> AbilityModifier::AttachEntity(void* entity,
                                                               uint32_t ability_handle) const {
  // Require a living entity with a modifier property.
  if (!EntityAlive(entity)) {
    return std::unexpected("the entity is not alive");
  }
  auto property = EntityProperty(entity);
  if (!property) {
    return std::unexpected(property.error());
  }

  // Replicate by marking the property predicted around AddModifier; pass the
  // parent ability subclass hash so embedded modifiers resolve ability values
  // even without an owned ability entity.
  const auto predicted = (*property)[predicted_];
  (*property)[predicted_] = 1;
  void* attached = add_(*property, entity, ability_handle, ability_subclass_, definition_, nullptr,
                        nullptr);
  (*property)[predicted_] = predicted;
  if (!attached) {
    return std::unexpected("modifier was not attached");
  }
  return {};
}

std::expected<bool, std::string> AbilityModifier::AttachedToEntity(const void* entity) const {
  auto property = EntityProperty(entity);
  if (!property) {
    return std::unexpected(property.error());
  }
  return HasDefinition(*property);
}

bool AbilityModifier::EntityAlive(const void* entity) const {
  if (!entity) {
    return false;
  }
  uint8_t life_state = 1;
  std::memcpy(&life_state, static_cast<const unsigned char*>(entity) + life_state_,
              sizeof(life_state));
  return life_state == 0;
}

void AbilityModifier::Restore() const {
  std::memcpy(definition_ + duration_, &previous_, sizeof(previous_));
}

std::expected<unsigned char*, std::string> AbilityModifier::Property(PawnObserver& observer,
                                                                     int32_t slot) const {
  // A live pawn holds the property.
  const auto sample = observer.Observe(slot);
  auto* pawn = static_cast<unsigned char*>(observer.PawnForSlot(slot));
  if (!sample || sample->health <= 0 || !pawn) {
    return std::unexpected("modifier pawn unavailable");
  }
  unsigned char* property = nullptr;
  std::memcpy(&property, pawn + property_, sizeof(property));
  if (!property) {
    return std::unexpected("modifier property unavailable");
  }
  return property;
}

std::expected<unsigned char*, std::string> AbilityModifier::EntityProperty(
    const void* entity) const {
  if (!entity) {
    return std::unexpected("the entity has no modifiers");
  }
  unsigned char* property = nullptr;
  std::memcpy(&property, static_cast<const unsigned char*>(entity) + property_, sizeof(property));
  if (!property) {
    return std::unexpected("the entity has no modifiers");
  }
  return property;
}

std::expected<bool, std::string> AbilityModifier::HasDefinition(
    const unsigned char* property) const {
  // The vector is a CUtlVector of modifier pointers.
  const auto rows = ReadUtlVector<unsigned char*>(property + modifiers_, kMaxModifiers);
  if (!rows) {
    return std::unexpected("modifier vector is invalid");
  }

  // Look for this definition among the modifiers.
  for (int32_t index = 0; index < rows->count; ++index) {
    if (!rows->data[index]) {
      continue;
    }
    // CBaseModifier's definition pointer is at +0x10, as in Deadworks
    // CBaseModifier.
    void* current = nullptr;
    std::memcpy(&current, rows->data[index] + 0x10, sizeof(current));
    if (current == definition_) {
      return true;
    }
  }
  return false;
}

}  // namespace modlock::gameinterop
