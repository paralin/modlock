//go:build deps_only

#include <iostream>

#include "wasm/game.h"

namespace modlock::wasm {

std::expected<void, std::string> Game::AssignTeams(const match::TeamAssignments& request) {
  return assignments_.Assign(request);
}

std::expected<void, std::string> Game::SeatAssignedPlayer(int32_t slot, uint32_t generation,
                                                          uint64_t steam_id, int32_t team) {
  // Wait for the controller, then move a hero without replacing it.
  const auto current = observer_.ControllerTeam(slot);
  if (!current) {
    return std::unexpected("player controller unavailable");
  }
  const auto hero = team != 1 && *current >= 2 ? observer_.Observe(slot) : std::nullopt;
  if (hero && hero->team >= 2 && hero->hero_id != 0) {
    if (*current == team) {
      return {};
    }
    auto change = services_.ChangeTeam();
    if (!change) {
      return std::unexpected(change.error());
    }
    return observer_.MovePlayer(slot, generation, team, *change);
  }

  // An observer needs a camera; a new player joins with the hero picker.
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }
  if (team == 1) {
    return observer_.SelectPlayer(slot, steam_id, generation, team, nullptr, **selection);
  }
  auto seated = observer_.SeatForChoice(slot, steam_id, generation, team, **selection, true);
  if (!seated) {
    return std::unexpected(seated.error());
  }
  return {};
}

void Game::SeatAssignedPlayers() {
  // Only pending humans touch the engine. The scan is bounded by the server's
  // player slots, with indexed roster lookups and no world scan.
  if (assignments_.Empty()) {
    return;
  }
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    // A connection without a host seat continues under the mod's rules.
    const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
    const auto assignment = assignments_.Pending(state.xuid, slot, state.generation);
    if (!state.occupied || state.is_bot || !state.fully_connected || !assignment || !Serves(slot)) {
      continue;
    }

    // Retry a missing controller or delayed pawn on the next engine frame.
    auto seated = SeatAssignedPlayer(slot, state.generation, state.xuid, *assignment);
    const auto current = gameinterop::ConnectionTracker::StateForSlot(slot);
    const bool same_connection =
        current.occupied && current.xuid == state.xuid && current.generation == state.generation;
    if (seated && same_connection && observer_.ControllerTeam(slot) == *assignment) {
      assignments_.Confirm(state.xuid, slot, state.generation, *assignment);
      seat_failed_.erase(slot);
    } else if (!seated && seat_failed_.insert(slot).second) {
      std::cerr << "team assignment: " << seated.error() << '\n';
    }
  }
}

}  // namespace modlock::wasm
