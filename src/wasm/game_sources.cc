//go:build deps_only

#include <array>
#include <optional>
#include <vector>

#include "wasm/game.h"

namespace modlock::wasm {

// The source interfaces the host-wide hooks read this game through. The game
// registers them while it lives (game.cc), so each hook sees only the answers
// below and none of the state behind them.

bool Game::KeepsMomentum() const {
  return projectiles_.keep_momentum();
}

std::vector<ProjectileSource::HeroMotion> Game::Motions() {
  std::vector<HeroMotion> motions;
  for (const auto& hero : players_.Heroes()) {
    if (hero.velocity) {
      motions.push_back({.pawn = hero.pawn, .velocity = *hero.velocity});
    }
  }
  return motions;
}

bool Game::Silences(int32_t concept_value) const {
  return silenced_.contains(concept_value);
}

bool Game::SilencesSpeaker(uint32_t speaker, int32_t concept_value) const {
  if (silenced_players_.empty()) {
    return false;
  }
  const auto slot = players_.Observer().SlotOfPawn(speaker);
  if (!slot) {
    return false;
  }
  const auto found = silenced_players_.find(*slot);
  return found != silenced_players_.end() && found->second.contains(concept_value);
}

}  // namespace modlock::wasm
