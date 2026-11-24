//go:build deps_only

#include <array>
#include <optional>
#include <vector>

#include "wasm/game.h"

namespace modlock::wasm {

// The source interfaces the host-wide hooks read this game through. The game
// registers them while it lives (game.cc), so each hook sees only the answers
// below and none of the state behind them.

gameinterop::CommandHook::Command* Game::DriveOf(int32_t slot) {
  const auto found = drives_.find(slot);
  return found == drives_.end() ? nullptr : &found->second;
}

movement::Steering* Game::SteerOf(int32_t slot) {
  const auto found = steers_.find(slot);
  return found == steers_.end() ? nullptr : &found->second;
}

bool Game::Moves() const {
  return Moving() || !steers_.empty() || !drives_.empty();
}

void Game::Aim(void* pawn, const std::array<float, 3>& angles) {
  static_cast<void>(players_.Observer().SetEyeAngles(pawn, angles));
}

std::optional<int32_t> Game::WatchedSlot(int32_t slot) const {
  const auto watched = watched_.find(slot);
  if (watched == watched_.end()) {
    return std::nullopt;
  }
  return players_.Observer().SlotOfPawn(watched->second);
}

void Game::Withhold(gameinterop::TransmitSet& set, const Sources<CameraSource>& cameras) {
  // A text object's entity changes with its words, so each update reads the
  // current handles.
  visibility_.Withhold(set, [this](uint32_t id) { return ObjectHandle(id); });

  // A player inside the apart players receives no hero from outside them, and
  // one outside receives none from inside. A watcher outside receives every
  // hero while their camera follows one inside, so the camera has a hero to
  // follow; the players inside still never receive the watcher's. The heroes
  // are read each update, since a respawn or a hero swap changes their
  // handles.
  if (!apart_ || set.Slot() < 0 || set.Slot() >= gameinterop::kMaxPlayerSlots) {
    return;
  }
  const bool inside = (apart_ >> set.Slot()) & 1;
  if (!inside && WatchesInside(cameras, set.Slot(), apart_)) {
    return;
  }
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    if (((apart_ >> slot) & 1) == inside) {
      continue;
    }
    if (const auto hero = players_.Observer().PawnHandleForSlot(slot)) {
      set.Withhold(*hero);
    }
  }
}

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
