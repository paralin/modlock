#include "modlock/gameinterop/ability_modifier.h"

#include <algorithm>
#include <cstring>

#include "modlock/gameinterop/ability_definitions.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/keyvalues.h"

namespace modlock::gameinterop {

std::expected<AbilityModifier, std::string> AbilityModifier::Resolve(const ModuleImage& server,
                                                                     void* schema,
                                                                     std::string_view key,
                                                                     float duration) {
  // Deadworks NativeAbility and CModifierProperty provide the native call
  // contract.
  auto lookup = ResolveSignature(server, "vdata.lookup-by-hash");
  auto add = ResolveSignature(server, "modifier.add");
  if (!lookup) return std::unexpected(lookup.error());
  if (!add) return std::unexpected(add.error());
  const auto property = SchemaFieldOf(schema, "server.dll", "CBaseEntity", "m_pModifierProp");
  const auto modifiers = SchemaFieldOf(schema, "server.dll", "CModifierProperty", "m_vecModifiers");
  const auto predicted =
      SchemaFieldOf(schema, "server.dll", "CModifierProperty", "m_bPredictedOwner");
  const auto lifetime = SchemaFieldOf(schema, "server.dll", "CModifierVData", "m_flDuration");
  if (!property || !modifiers || !predicted || !lifetime)
    return std::unexpected("modifier property schema unavailable");
  const auto find = reinterpret_cast<AbilityDefinitions::Lookup>(*lookup);
  auto* definition = static_cast<unsigned char*>(find(2, MakeMemberName(key).hash));
  if (!definition)
    return std::unexpected("modifier definition " + std::string(key) + " unavailable");
  AbilityModifier modifier;
  std::memcpy(&modifier.previous_, definition + lifetime->offset, sizeof(modifier.previous_));
  std::memcpy(definition + lifetime->offset, &duration, sizeof(duration));
  modifier.add_ = reinterpret_cast<AddModifier>(*add);
  modifier.definition_ = definition;
  modifier.duration_ = lifetime->offset;
  modifier.property_ = property->offset;
  modifier.modifiers_ = modifiers->offset;
  modifier.predicted_ = predicted->offset;
  return modifier;
}

std::expected<void, std::string> AbilityModifier::Attach(PawnObserver& observer, int32_t slot,
                                                         uint32_t ability) const {
  auto property = Property(observer, slot);
  if (!property) return std::unexpected(property.error());
  const auto sample = observer.Observe(slot);
  auto owned = observer.CurrentAbilitiesForSlot(slot);
  if (!sample || !owned) return std::unexpected("modifier pawn unavailable");
  const auto source = std::ranges::find(*owned, ability, &PawnObserver::Ability::subclass_id);
  if (source == owned->end()) return std::unexpected("modifier source ability unavailable");

  // The native call replicates only from a predicted owner.
  auto* pawn = observer.PawnForSlot(slot);
  const auto predicted = (*property)[predicted_];
  (*property)[predicted_] = 1;
  void* attached =
      add_(*property, pawn, source->handle, sample->team, definition_, nullptr, nullptr);
  (*property)[predicted_] = predicted;
  if (!attached) return std::unexpected("modifier was not attached");
  return {};
}

std::expected<bool, std::string> AbilityModifier::Attached(PawnObserver& observer,
                                                           int32_t slot) const {
  auto property = Property(observer, slot);
  if (!property) return std::unexpected(property.error());
  struct PointerVector {
    int32_t count;
    uint32_t padding;
    unsigned char** data;
    uint32_t capacity;
    uint32_t flags;
  } rows{};
  std::memcpy(&rows, *property + modifiers_, sizeof(rows));
  if (rows.count < 0 || rows.count > 1024 || uint32_t(rows.count) > rows.capacity ||
      (rows.count && !rows.data))
    return std::unexpected("modifier vector is invalid");
  for (int32_t index = 0; index < rows.count; ++index) {
    if (!rows.data[index]) continue;
    // CBaseModifier's definition pointer is at +0x10, as in Deadworks
    // CBaseModifier.
    void* current = nullptr;
    std::memcpy(&current, rows.data[index] + 0x10, sizeof(current));
    if (current == definition_) return true;
  }
  return false;
}

void AbilityModifier::Restore() const {
  std::memcpy(definition_ + duration_, &previous_, sizeof(previous_));
}

std::expected<unsigned char*, std::string> AbilityModifier::Property(PawnObserver& observer,
                                                                     int32_t slot) const {
  const auto sample = observer.Observe(slot);
  auto* pawn = static_cast<unsigned char*>(observer.PawnForSlot(slot));
  if (!sample || sample->health <= 0 || !pawn) return std::unexpected("modifier pawn unavailable");
  unsigned char* property = nullptr;
  std::memcpy(&property, pawn + property_, sizeof(property));
  if (!property) return std::unexpected("modifier property unavailable");
  return property;
}

}  // namespace modlock::gameinterop
