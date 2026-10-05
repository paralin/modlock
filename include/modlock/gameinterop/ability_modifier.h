#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/pawn_observer.h"

namespace modlock::gameinterop {

// AbilityModifier attaches one ability's modifier definition to a player, as if
// the player's own copy of that ability applied it, for a fixed duration. The
// definition belongs to the loaded world and is shared with every other user of
// that ability, so its duration applies to them too. Calls run on the engine
// thread; pawn pointers are borrowed only for one call.
class MODLOCK_API AbilityModifier {
 public:
  // Resolve finds the "<ability>/<modifier>" definition and sets its duration;
  // a negative duration never expires.
  static std::expected<AbilityModifier, std::string> Resolve(const ModuleImage& server,
                                                             void* schema, std::string_view key,
                                                             float duration);

  // Attach adds the modifier to a living player's pawn, sourced from the owned
  // ability with the given subclass identifier.
  std::expected<void, std::string> Attach(PawnObserver& observer, int32_t slot,
                                          uint32_t ability) const;

  // Attached reports whether a living player's pawn carries the modifier.
  std::expected<bool, std::string> Attached(PawnObserver& observer, int32_t slot) const;

  // Restore gives the definition back the duration it had before Resolve.
  void Restore() const;

 private:
  using AddModifier = void* (*)(void*, void*, uint32_t, int32_t, void*, void*, void*);

  // Property returns the modifier property of slot's living pawn.
  std::expected<unsigned char*, std::string> Property(PawnObserver& observer, int32_t slot) const;

  AddModifier add_ = nullptr;
  unsigned char* definition_ = nullptr;
  size_t duration_ = 0;
  float previous_ = 0;
  size_t property_ = 0;
  size_t modifiers_ = 0;
  size_t predicted_ = 0;
};

}  // namespace modlock::gameinterop
