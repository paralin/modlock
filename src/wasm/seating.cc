//go:build deps_only

#include "wasm/seating.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/gameinterop/connection_tracker.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

Seating::Seating(GameServices& services, Roster& players, ModifierHolds& modifiers, Serves serves)
    : services_(services), players_(players), modifiers_(modifiers), serves_(std::move(serves)) {}

void Seating::Frame() {
  SeatPlayers();
  ApplyFreeForAll();
}

void Seating::WorldEnding() {
  // Drop the per-world player state and reapply host seats on the next map.
  assignments_.WorldEnding();
  ffa_held_.clear();
}

void Seating::Leave(int32_t slot) {
  seat_failed_.erase(slot);
}

void Seating::Clear() {
  free_for_all_ = false;
  team_seating_ = false;
  ReleaseFreeForAll();
}

void Seating::StartFreeForAll(bool enabled) {
  free_for_all_ = enabled;
  team_seating_ = false;
  if (free_for_all_) {
    EnableRelations();
  }
}

void Seating::EnableRelations() {
  // Without the relations only bullets reach teammates; the match still runs.
  if (auto relations = services_.Relations()) {
    (*relations)->SetEnabled(true);
  } else {
    std::cerr << "free-for-all relations: " << relations.error() << '\n';
  }
}

std::expected<void, std::string> Seating::SetFreeForAll(const wasm::SetFreeForAllRequest& request) {
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

std::expected<void, std::string> Seating::SetTeamSeating(
    const wasm::SetTeamSeatingRequest& request) {
  team_seating_ = request.enabled();
  return {};
}

bool Seating::FreeForAllCommand(std::string_view line) const {
  // Only changeteam is blocked, and only in free-for-all.
  if (!free_for_all_) {
    return false;
  }
  const auto name = line.substr(0, line.find_first_of(" \t"));
  return name == "changeteam";
}

void Seating::SeatPlayers() {
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
      if (!state.occupied || !serves_(slot)) {
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

void Seating::ApplyFreeForAll() {
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

void Seating::ReleaseFreeForAll() {
  services_.DisableRelations();
  static constexpr const char* kFriendlyFire = "MODIFIER_STATE_FRIENDLY_FIRE_ENABLED";
  for (const auto handle : ffa_held_) {
    modifiers_.ReleaseState(handle, kFriendlyFire);
  }
  ffa_held_.clear();
  seat_failed_.clear();
}

std::expected<void, std::string> Seating::AssignTeams(const wasm::TeamAssignments& request) {
  return assignments_.Assign(request);
}

std::expected<void, std::string> Seating::SeatAssignedPlayer(int32_t slot, uint32_t generation,
                                                             uint64_t steam_id, int32_t team) {
  // Wait for the controller, then move a hero without replacing it.
  const auto current = players_.Observer().ControllerTeam(slot);
  if (!current) {
    return std::unexpected("player controller unavailable");
  }
  const auto hero = team != 1 && *current >= 2 ? players_.Observer().Observe(slot) : std::nullopt;
  if (hero && hero->team >= 2 && hero->hero_id != 0) {
    if (*current == team) {
      return {};
    }
    auto change = services_.ChangeTeam();
    if (!change) {
      return std::unexpected(change.error());
    }
    return players_.Observer().MovePlayer(slot, generation, team, *change);
  }

  // An observer needs a camera; a new player joins with the hero picker.
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }
  if (team == 1) {
    return players_.Observer().SelectPlayer(slot, steam_id, generation, team, nullptr, **selection);
  }
  auto seated =
      players_.Observer().SeatForChoice(slot, steam_id, generation, team, **selection, true);
  if (!seated) {
    return std::unexpected(seated.error());
  }
  return {};
}

void Seating::SeatAssignedPlayers() {
  // Only pending humans touch the engine. The scan is bounded by the server's
  // player slots, with indexed roster lookups and no world scan.
  if (assignments_.Empty()) {
    return;
  }
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    // A connection without a host seat continues under the mod's rules.
    const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
    const auto assignment = assignments_.Pending(state.xuid, slot, state.generation);
    if (!state.occupied || state.is_bot || !state.fully_connected || !assignment ||
        !serves_(slot)) {
      continue;
    }

    // Retry a missing controller or delayed pawn on the next engine frame.
    auto seated = SeatAssignedPlayer(slot, state.generation, state.xuid, *assignment);
    const auto current = gameinterop::ConnectionTracker::StateForSlot(slot);
    const bool same_connection =
        current.occupied && current.xuid == state.xuid && current.generation == state.generation;
    if (seated && same_connection && players_.Observer().ControllerTeam(slot) == *assignment) {
      assignments_.Confirm(state.xuid, slot, state.generation, *assignment);
      seat_failed_.erase(slot);
    } else if (!seated && seat_failed_.insert(slot).second) {
      std::cerr << "team assignment: " << seated.error() << '\n';
    }
  }
}

}  // namespace modlock::wasm
