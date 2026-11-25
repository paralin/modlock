//go:build deps_only

#include "wasm/game.h"

#include <ranges>

namespace modlock::wasm {

Game::Game(GameServices& services, EngineHost* engine) : services_(services), engine_(engine) {}

Game::~Game() = default;

HostCalls Game::Calls() {
  return {
      .players = &players_,
      .world = &world_,
      .match = &match_,
      .fields = &fields_,
      .modifiers = &modifiers_,
      .heroes = &heroes_,
      .camera = &camera_,
      .movement = &movement_,
      .input = &input_,
      .projectiles = &projectiles_,
      .restores = &restores_,
      .seating = &seating_,
      .voice = &voice_,
      .effects = &effects_,
  };
}

void Game::Frame() {
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
  static_cast<void>(services_.EntityCalls());
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
  // Release what the mod held in the world.
  for (auto* module : std::views::reverse(modules_)) {
    module->WorldEnding();
  }
}

void Game::Leave(int32_t slot) {
  for (auto* module : std::views::reverse(modules_)) {
    module->Leave(slot);
  }
}

void Game::Clear() {
  // Release what each module holds.
  for (auto* module : std::views::reverse(modules_)) {
    module->Clear();
  }
}

}  // namespace modlock::wasm
