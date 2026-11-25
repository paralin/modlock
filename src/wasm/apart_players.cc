//go:build deps_only

#include "wasm/apart_players.h"

#include "wasm/game.h"

namespace modlock::wasm {

ApartPlayers::ApartPlayers(GameServices& services, const Roster& players)
    : services_(services), players_(players) {
  services_.TransmitSources().Add(this);
}

ApartPlayers::~ApartPlayers() {
  services_.TransmitSources().Remove(this);
}

std::expected<void, std::string> ApartPlayers::Set(uint64_t slots) {
  // The engine sends only what its transmit pass allows, so keeping players
  // apart needs the shared transmit hook.
  apart_ = slots;
  return slots ? services_.Transmit() : std::expected<void, std::string>{};
}

void ApartPlayers::Withhold(gameinterop::TransmitSet& set, const Sources<CameraSource>& cameras) {
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

}  // namespace modlock::wasm
