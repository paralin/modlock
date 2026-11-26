#pragma once

#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// GameDataGaps lists the recorded entries of the game data file that the
// running game lacks. Each one disables only the features that need it.
struct GameDataGaps {
  // signatures are the unresolved signatures, in id order.
  std::vector<GameDataGap> signatures;
  // slots are the unresolved virtual slots, in id order.
  std::vector<GameDataGap> slots;

  // empty reports whether every recorded entry resolved.
  bool empty() const { return signatures.empty() && slots.empty(); }
};

// ResolveGameData resolves every recorded signature and virtual slot of each
// loaded game module and returns the entries that did not resolve. An entry
// of a module that is not loaded yet is neither resolved nor a gap, so a call
// once the engine has started covers every module. It logs each gap the
// first time a call finds it. Every result is kept for the life of the
// process, so a later call, or a later lookup of any entry, answers without
// a scan. Run it first once the host maps the game's modules, before any hook
// rewrites code a pattern reads. Off Windows there is no game to resolve
// against, and it returns no gaps.
MODLOCK_API GameDataGaps ResolveGameData();

}  // namespace modlock::gameinterop
