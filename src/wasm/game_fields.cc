//go:build deps_only

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

void Game::StartFreeForAll(bool enabled) {
  free_for_all_ = enabled;
  team_seating_ = false;
  if (free_for_all_) {
    EnableRelations();
  }
}

void Game::EnableRelations() {
  // Without the relations only bullets reach teammates; the match still runs.
  if (auto relations = services_.Relations()) {
    (*relations)->SetEnabled(true);
  } else {
    std::cerr << "free-for-all relations: " << relations.error() << '\n';
  }
}

std::expected<void, std::string> Game::SetFreeForAll(const wasm::SetFreeForAllRequest& request) {
  if (free_for_all_ == request.enabled()) {
    return {};
  }
  free_for_all_ = request.enabled();
  if (free_for_all_) {
    EnableRelations();
  } else {
    ReleaseFreeForAll();
  }
  return {};
}

std::expected<void, std::string> Game::SetTeamSeating(const wasm::SetTeamSeatingRequest& request) {
  team_seating_ = request.enabled();
  return {};
}

bool Game::FreeForAllCommand(std::string_view line) const {
  // Only changeteam is blocked, and only in free-for-all.
  if (!free_for_all_) {
    return false;
  }
  const auto name = line.substr(0, line.find_first_of(" \t"));
  return name == "changeteam";
}

void Game::SeatPlayers() {
  SeatAssignedPlayers();

  // Seat players only while seating or free-for-all is on.
  if (!free_for_all_ && !team_seating_) {
    return;
  }
  auto selection = services_.Selection();
  if (selection) {
    // Seat each player with no team on the side with fewer players.
    std::array<int, 4> sides{};
    std::vector<int32_t> unseated;
    for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
      // Count each side and collect the humans without a team.
      const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
      if (!state.occupied || !Serves(slot)) {
        continue;
      }
      const auto team = players_.Observer().ControllerTeam(slot);
      if (!team) {
        continue;
      }
      if (*team == 0 && !state.is_bot && state.xuid != 0 && !assignments_.Contains(state.xuid)) {
        unseated.push_back(slot);
      } else if (*team < sides.size()) {
        ++sides[*team];
      }
    }
    for (const auto slot : unseated) {
      const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
      const int team = sides[3] < sides[2] ? 3 : 2;
      auto seated =
          players_.Observer().SeatForChoice(slot, state.xuid, state.generation, team, **selection);
      if (seated && *seated) {
        ++sides[team];
        seat_failed_.erase(slot);
      } else if (!seated && seat_failed_.insert(slot).second) {
        std::cerr << "team seating: " << seated.error() << '\n';
      }
    }
  } else if (seat_failed_.insert(-1).second) {
    std::cerr << "team seating: " << selection.error() << '\n';
  }
}

void Game::ApplyFreeForAll() {
  // Hold friendly fire only in free-for-all.
  if (!free_for_all_) {
    return;
  }
  static constexpr const char* kFriendlyFire = "MODIFIER_STATE_FRIENDLY_FIRE_ENABLED";

  // Hold friendly fire on each living hero, and let a gone hero's hold go.
  std::set<uint32_t> living;
  for (const auto& hero : players_.Heroes()) {
    living.insert(hero.handle);
    if (ffa_held_.insert(hero.handle).second) {
      modifiers_.KeepState(hero.handle, kFriendlyFire);
    }
  }
  std::erase_if(ffa_held_, [&](uint32_t handle) {
    if (living.contains(handle)) {
      return false;
    }
    modifiers_.ForgetState(handle, kFriendlyFire);
    return true;
  });
}

void Game::ReleaseFreeForAll() {
  if (services_.relations_) {
    services_.relations_->SetEnabled(false);
  }
  static constexpr const char* kFriendlyFire = "MODIFIER_STATE_FRIENDLY_FIRE_ENABLED";
  for (const auto handle : ffa_held_) {
    modifiers_.ReleaseState(handle, kFriendlyFire);
  }
  ffa_held_.clear();
  seat_failed_.clear();
}

}  // namespace modlock::wasm
