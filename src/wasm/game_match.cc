//go:build deps_only

#include <iostream>

#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

std::expected<void, std::string> Game::Pause(const wasm::PauseRequest& request) {
  return SetPaused(request.paused());
}

std::expected<void, std::string> Game::SetPaused(bool paused) {
  // Pause or resume the game, and remember it.
  auto pause = services_.Pause();
  if (!pause) {
    return std::unexpected(pause.error());
  }
  if (auto set = (*pause)->Set(paused); !set) {
    return set;
  }
  paused_ = paused;
  return {};
}

std::expected<wasm::MatchClockResponse, std::string> Game::MatchClock() {
  // Resolve the match clock and the server's time.
  auto clock = services_.Clock();
  if (!clock) {
    return std::unexpected(clock.error());
  }
  auto server = services_.Server();
  if (!server) {
    return std::unexpected(server.error());
  }
  auto now = (*server)->ReadClock();
  if (!now) {
    return std::unexpected(now.error());
  }

  // Read the match clock in seconds.
  auto seconds = (*clock)->Read(now->current_time, now->interval);
  if (!seconds) {
    return std::unexpected(seconds.error());
  }
  wasm::MatchClockResponse response;
  response.set_seconds(*seconds);
  return response;
}

std::expected<void, std::string> Game::HoldMatchClock(const wasm::HoldMatchClockRequest& request) {
  // No seconds releases the hold.
  if (!request.has_seconds()) {
    held_clock_.reset();
    return {};
  }

  // Hold the clock at the requested seconds.
  if (auto clock = services_.Clock(); !clock) {
    return std::unexpected(clock.error());
  }
  held_clock_ = request.seconds();
  HoldClock();
  return {};
}

void Game::HoldClock() {
  // Read the clock while a hold is set.
  if (!held_clock_) {
    return;
  }
  auto clock = services_.Clock();
  auto server = services_.Server();
  if (!clock || !server) {
    return;
  }
  auto now = (*server)->ReadClock();
  if (!now) {
    return;
  }

  // A hold the game refuses ends with a log line instead of every frame.
  auto held = (*clock)->Restore(*held_clock_, now->current_time, now->interval);
  if (held) {
    held = ReplicateClock();
  }
  if (!held) {
    std::cerr << "[modlock] match clock hold ended: " << held.error() << "\n";
    held_clock_.reset();
  }
}

std::expected<void, std::string> Game::ReplicateClock() {
  auto rules = Entity(rules_entity_);
  if (!rules) {
    // Find the rules entity once.
    auto world = World();
    if (!world) {
      return std::unexpected(world.error());
    }
    auto found = (*world)->Find("citadel_gamerules");
    if (!found) {
      return std::unexpected(found.error());
    }
    if (found->empty()) {
      return std::unexpected("the game has no rules entity");
    }
    rules_entity_ = found->front();
    rules = Entity(rules_entity_);
    if (!rules) {
      return std::unexpected(rules.error());
    }
  }
  if (!gameinterop::NotifyEntityStateChanged(*rules)) {
    return std::unexpected("match clock replication is unavailable");
  }
  return {};
}

std::expected<wasm::RiftResponse, std::string> Game::Rift() {
  // Read the rift's state.
  auto rift = services_.Rift();
  if (!rift) {
    return std::unexpected(rift.error());
  }
  auto state = (*rift)->Read();
  if (!state) {
    return std::unexpected(state.error());
  }

  // Copy the scoring state into the response.
  wasm::RiftResponse response;
  auto* out = response.mutable_state();
  out->set_scoring_team(state->scoring_team);
  out->set_scoring_time(state->scoring_time);
  out->set_cash_in_started(state->cash_in_started);

  // Copy the spawn timing.
  out->set_give_up_time(state->give_up_time);
  out->set_next_spawn(state->next_spawn);
  out->set_spawn_window(state->spawn_window);
  return response;
}

std::expected<void, std::string> Game::StartRift(const wasm::StartRiftRequest& request) {
  auto rift = services_.Rift();
  if (!rift) {
    return std::unexpected(rift.error());
  }
  return (*rift)->StartAt(Floats(request.position()));
}

}  // namespace modlock::wasm
