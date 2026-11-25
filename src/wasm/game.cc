//go:build deps_only

#include "wasm/game.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>
#include <vector>

#include "modlock/gameinterop/connection_tracker.h"
#include "wasm/game_internal.h"
#include "wasm/modifier_masks.h"

namespace modlock::wasm {

Game::Game(GameServices& services, EngineHost* engine) : services_(services), engine_(engine) {
  // Register with the host-wide hooks while the game lives.
  services_.VoiceSources().Add(this);
}

Game::~Game() {
  services_.VoiceSources().Remove(this);
}

void Game::Frame() {
  // Apply the per-frame state each feature keeps.
  SeatPlayers();
  ApplyFreeForAll();

  // Run the modules, so the world finishes the units spawned this frame
  // together.
  for (auto* module : std::views::reverse(modules_)) {
    module->Frame();
  }
}

void Game::WorldReady() {
  // Each lookup is kept once found; one that fails here is reported by the
  // call that needs it.
  static_cast<void>(services_.Heroes());
  static_cast<void>(services_.WorldCalls());
  static_cast<void>(services_.Selection());

  // Find every recorded server function now. Each is a scan of the whole
  // module, which would otherwise run inside the first call that needs it.
  if (const auto image = services_.Image()) {
    gameinterop::ResolveModuleSignatures(**image, gameinterop::GameModule::kServer);
  }
  for (auto* module : modules_) {
    module->WorldReady();
  }
}

void Game::WorldEnding() {
  // Drop the per-world player state and reapply host seats on the next map.
  assignments_.WorldEnding();
  ffa_held_.clear();

  // Release what the mod held in the world.
  for (auto* module : std::views::reverse(modules_)) {
    module->WorldEnding();
  }
}

void Game::Leave(int32_t slot) {
  // Forget the slot's seat.
  seat_failed_.erase(slot);

  // Forget its silences.
  silenced_players_.erase(slot);

  for (auto* module : std::views::reverse(modules_)) {
    module->Leave(slot);
  }
}

void Game::Clear() {
  // Release the free-for-all and team seating.
  free_for_all_ = false;
  team_seating_ = false;
  ReleaseFreeForAll();

  // Release the voices it silenced.
  silenced_.clear();
  silenced_players_.clear();

  // Release what each module holds.
  for (auto* module : std::views::reverse(modules_)) {
    module->Clear();
  }
}

std::expected<void, std::string> Game::Pause(const wasm::PauseRequest& request) {
  return match_.Pause(request);
}

std::expected<wasm::MatchClockResponse, std::string> Game::MatchClock() {
  return match_.MatchClock();
}

std::expected<void, std::string> Game::HoldMatchClock(const wasm::HoldMatchClockRequest& request) {
  return match_.HoldMatchClock(request);
}

std::expected<wasm::RiftResponse, std::string> Game::Rift() {
  return match_.Rift();
}

std::expected<void, std::string> Game::StartRift(const wasm::StartRiftRequest& request) {
  return match_.StartRift(request);
}

std::expected<void, std::string> Game::ServerCommand(const ServerCommandRequest& request) {
  return services_.ServerCommand(request.command());
}

std::expected<void, std::string> Game::Chat(const effects::ChatRequest& request) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->Chat(request.player(), request.text());
}

std::expected<void, std::string> Game::CenterText(const effects::CenterTextRequest& request) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->CenterText(request.player(), request.text());
}

std::expected<void, std::string> Game::Announce(const effects::AnnounceRequest& request) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->Announce(request.player(), request.title(), request.text());
}

}  // namespace modlock::wasm
