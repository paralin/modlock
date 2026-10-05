#include "modlock/gameinterop/ability_slots.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"

namespace modlock::gameinterop {

std::expected<AbilitySlots, std::string> AbilitySlots::Resolve(const ModuleImage& server,
                                                               void* schema) {
  const auto component =
      SchemaFieldOf(schema, "server.dll", "CCitadelPlayerPawn", "m_CCitadelAbilityComponent");
  const auto abilities =
      SchemaFieldOf(schema, "server.dll", "CCitadelAbilityComponent", "m_vecAbilities");
  const auto thinkable =
      SchemaFieldOf(schema, "server.dll", "CCitadelAbilityComponent", "m_vecThinkableAbilities");
  if (!component || !abilities) return std::unexpected("native ability slot schema unavailable");
  auto find = ResolveSignature(server, "ability.find-slot");
  auto detach = ResolveSignature(server, "ability.detach-slot");
  auto remove = ResolveSignature(server, "entity.remove");
  if (!find) return std::unexpected(find.error());
  if (!detach) return std::unexpected(detach.error());
  if (!remove) return std::unexpected(remove.error());
  AbilitySlots result;
  result.component_ = component->offset;
  result.vectors_ = {abilities->offset, thinkable ? thinkable->offset : 0};
  result.vector_count_ = thinkable ? 2 : 1;
  result.find_ = reinterpret_cast<decltype(result.find_)>(*find);
  result.detach_ = reinterpret_cast<decltype(result.detach_)>(*detach);
  result.remove_ = reinterpret_cast<decltype(result.remove_)>(*remove);
  return result;
}

std::expected<void, std::string> AbilitySlots::Replace(PawnObserver& observer, int32_t player,
                                                       uint16_t slot, uint32_t subclass,
                                                       const AbilityDefinitions& definitions,
                                                       CreateAbility create) const {
  if (!observer.Observe(player)) return std::unexpected("native ability pawn unavailable");
  auto owned = observer.CurrentAbilitiesForSlot(player);
  if (!owned) return std::unexpected(owned.error());
  auto current = std::ranges::find(*owned, slot, &PawnObserver::Ability::slot);
  if (current != owned->end() && current->subclass_id == subclass) return {};
  auto definition = definitions.Find(subclass);
  if (!definition) return std::unexpected(definition.error());
  if (auto removed = Remove(observer, player, slot); !removed) return removed;
  auto* pawn = static_cast<unsigned char*>(observer.PawnForSlot(player));
  if (!pawn || !create) return std::unexpected("native ability creation unavailable");
  create(pawn + component_, definition->native_definition_pointer, slot, 0, true, nullptr);
  owned = observer.CurrentAbilitiesForSlot(player);
  if (!owned) return std::unexpected(owned.error());
  current = std::ranges::find(*owned, slot, &PawnObserver::Ability::slot);
  if (current == owned->end() || current->subclass_id != subclass)
    return std::unexpected("native ability replacement ownership readback differs");
  return {};
}

std::expected<void, std::string> AbilitySlots::Remove(PawnObserver& observer, int32_t player,
                                                      uint16_t slot) const {
  if (!observer.Observe(player)) return std::unexpected("native ability pawn unavailable");
  auto owned = observer.CurrentAbilitiesForSlot(player);
  if (!owned) return std::unexpected(owned.error());
  auto current = std::ranges::find(*owned, slot, &PawnObserver::Ability::slot);
  if (current == owned->end()) return {};
  auto* pawn = static_cast<unsigned char*>(observer.PawnForSlot(player));
  if (!pawn) return std::unexpected("native ability pawn unavailable");
  auto* component = pawn + component_;
  auto entities = ResolveLiveEntitySystem();
  if (!entities) return std::unexpected(entities.error());
  void* ability = EntityInstance(*entities, current->handle);
  if (!ability) return std::unexpected("native ability entity unavailable");
  // CUtlVector<uint32_t> and OnAbilityRemoved's slot table are the native
  // indexes used by Deadworks RemoveAbilityImpl. Validate before detaching.
  struct Handles {
    int32_t count;
    uint32_t padding;
    uint32_t* data;
    uint32_t capacity;
    uint32_t flags;
  };
  static_assert(sizeof(Handles) == 24);
  std::array<Handles, 2> vectors{};
  for (size_t i = 0; i < vector_count_; ++i) {
    auto& vector = vectors[i];
    std::memcpy(&vector, component + vectors_[i], sizeof(vector));
    if (vector.count < 0 || vector.count > 256 || uint32_t(vector.count) > vector.capacity ||
        (vector.count && !vector.data))
      return std::unexpected("native ability vector is invalid");
  }
  const int32_t entry = find_(component + 0x30, &slot);
  if (entry < 0) return std::unexpected("native ability slot table entry unavailable");
  detach_(component + 0x30, entry);
  for (size_t i = 0; i < vector_count_; ++i) {
    auto& vector = vectors[i];
    for (int32_t index = 0; index < vector.count; ++index) {
      if (vector.data[index] != current->handle) continue;
      std::memmove(vector.data + index, vector.data + index + 1,
                   (vector.count - index - 1) * sizeof(uint32_t));
      --vector.count;
      std::memcpy(component + vectors_[i], &vector.count, sizeof(vector.count));
      break;
    }
  }
  if (!NotifyEntityStateChanged(pawn))
    return std::unexpected("native ability removal could not be replicated");
  remove_(ability);
  return {};
}

}  // namespace modlock::gameinterop
