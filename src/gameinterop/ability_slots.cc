#include "modlock/gameinterop/ability_slots.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"
#include "utl_vector.h"

namespace modlock::gameinterop {
namespace {

// kSlotTableOffset locates the ability component's slot table, which the
// find-slot and detach-slot calls take, as Deadworks RemoveAbilityImpl reads it.
constexpr size_t kSlotTableOffset = 0x30;

}  // namespace

std::expected<AbilitySlots, std::string> AbilitySlots::Resolve(const ModuleImage& server,
                                                               void* schema) {
  // Resolve the ability component's schema fields.
  const auto component =
      SchemaFieldOf(schema, "server.dll", "CCitadelPlayerPawn", "m_CCitadelAbilityComponent");
  const auto abilities =
      SchemaFieldOf(schema, "server.dll", "CCitadelAbilityComponent", "m_vecAbilities");
  const auto thinkable =
      SchemaFieldOf(schema, "server.dll", "CCitadelAbilityComponent", "m_vecThinkableAbilities");
  if (!component || !abilities) {
    return std::unexpected("native ability slot schema unavailable");
  }

  // Resolve the slot calls.
  AbilitySlots result;
  const SignatureCall signatures[] = {
      {"ability.find-slot", &result.find_},
      {"ability.detach-slot", &result.detach_},
      {"entity.remove", &result.remove_},
  };
  auto resolved = ResolveSignatures(server, signatures);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  // Keep the offsets.
  result.component_ = component->offset;
  result.vectors_ = {abilities->offset, thinkable ? thinkable->offset : 0};
  result.vector_count_ = thinkable ? 2 : 1;
  return result;
}

std::expected<void, std::string> AbilitySlots::Replace(PawnObserver& observer, int32_t player,
                                                       uint16_t slot, uint32_t subclass,
                                                       uint16_t upgrades,
                                                       const AbilityDefinitions& definitions,
                                                       CreateAbility create) const {
  // Find the ability in the slot; a match needs no change.
  if (!observer.Observe(player)) {
    return std::unexpected("native ability pawn unavailable");
  }
  auto owned = observer.CurrentAbilitiesForSlot(player);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  auto current = std::ranges::find(*owned, slot, &PawnObserver::Ability::slot);
  if (current != owned->end() && current->subclass_id == subclass) {
    return {};
  }

  // Remove the old ability and create the new one.
  auto definition = definitions.Find(subclass);
  if (!definition) {
    return std::unexpected(definition.error());
  }
  if (auto removed = Remove(observer, player, slot); !removed) {
    return removed;
  }
  auto* pawn = static_cast<unsigned char*>(observer.PawnForSlot(player));
  if (!pawn || !create) {
    return std::unexpected("native ability creation unavailable");
  }
  create(pawn + component_, definition->native_definition_pointer, slot, upgrades, true, nullptr);

  // Read back the new ownership.
  owned = observer.CurrentAbilitiesForSlot(player);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  current = std::ranges::find(*owned, slot, &PawnObserver::Ability::slot);
  if (current == owned->end() || current->subclass_id != subclass) {
    return std::unexpected("native ability replacement ownership readback differs");
  }
  return {};
}

std::expected<void, std::string> AbilitySlots::Remove(PawnObserver& observer, int32_t player,
                                                      uint16_t slot) const {
  // Find the ability in the slot; an empty slot needs no change.
  if (!observer.Observe(player)) {
    return std::unexpected("native ability pawn unavailable");
  }
  auto owned = observer.CurrentAbilitiesForSlot(player);
  if (!owned) {
    return std::unexpected(owned.error());
  }
  auto current = std::ranges::find(*owned, slot, &PawnObserver::Ability::slot);
  if (current == owned->end()) {
    return {};
  }

  // Find the pawn's component and the ability's entity.
  auto* pawn = static_cast<unsigned char*>(observer.PawnForSlot(player));
  if (!pawn) {
    return std::unexpected("native ability pawn unavailable");
  }
  auto* component = pawn + component_;
  auto entities = ResolveLiveEntitySystem();
  if (!entities) {
    return std::unexpected(entities.error());
  }
  void* ability = EntityInstance(*entities, current->handle);
  if (!ability) {
    return std::unexpected("native ability entity unavailable");
  }

  // The handle vectors and OnAbilityRemoved's slot table are the native
  // indexes used by Deadworks RemoveAbilityImpl. Validate before detaching.
  std::array<UtlVector<uint32_t>, 2> vectors{};
  for (size_t i = 0; i < vector_count_; ++i) {
    auto vector = ReadUtlVector<uint32_t>(component + vectors_[i], kMaxAbilities);
    if (!vector) {
      return std::unexpected("native ability vector is invalid");
    }
    vectors[i] = *vector;
  }

  // Detach the slot table entry.
  const int32_t entry = find_(component + kSlotTableOffset, &slot);
  if (entry < 0) {
    return std::unexpected("native ability slot table entry unavailable");
  }
  detach_(component + kSlotTableOffset, entry);

  // Drop the handle from each vector, replicate, and remove the entity.
  for (size_t i = 0; i < vector_count_; ++i) {
    auto& vector = vectors[i];
    for (int32_t index = 0; index < vector.count; ++index) {
      // Remove the handle where it appears.
      if (vector.data[index] != current->handle) {
        continue;
      }
      std::memmove(vector.data + index, vector.data + index + 1,
                   (vector.count - index - 1) * sizeof(uint32_t));
      --vector.count;
      std::memcpy(component + vectors_[i], &vector.count, sizeof(vector.count));
      break;
    }
  }
  if (!NotifyEntityStateChanged(pawn)) {
    return std::unexpected("native ability removal could not be replicated");
  }
  remove_(ability);
  return {};
}

}  // namespace modlock::gameinterop
