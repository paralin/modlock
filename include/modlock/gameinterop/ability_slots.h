#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/pawn_observer.h"

namespace modlock::gameinterop {

// AbilitySlots replaces native abilities without item-specific removal callbacks.
// Resolved functions and schema belong to one loaded server world. Calls run on
// the engine thread.
class MODLOCK_API AbilitySlots {
 public:
  static std::expected<AbilitySlots, std::string> Resolve(const ModuleImage& server, void* schema);

  // Replace puts the ability subclass in a player's ability slot, removing the
  // ability there. A slot that already holds subclass is left alone.
  std::expected<void, std::string> Replace(PawnObserver& observer, int32_t player, uint16_t slot,
                                           uint32_t subclass, const AbilityDefinitions& definitions,
                                           CreateAbility create) const;

  // Remove takes the ability out of a player's ability slot; an empty slot is
  // left alone.
  std::expected<void, std::string> Remove(PawnObserver& observer, int32_t player,
                                          uint16_t slot) const;

 private:
  size_t component_ = 0;
  std::array<size_t, 2> vectors_{};
  // Game build 6711 removed m_vecThinkableAbilities; only present vectors count.
  size_t vector_count_ = 0;
  int32_t (*find_)(void*, uint16_t*) = nullptr;
  void (*detach_)(void*, int32_t) = nullptr;
  void (*remove_)(void*) = nullptr;
};

}  // namespace modlock::gameinterop
