//go:build deps_only

#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

// The input calls replace the mod's changes; the shared hook applies them.

std::expected<void, std::string> Game::BlockInput(const wasm::InputRequest& request) {
  input_.Block(request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::BlockPlayerInput(const wasm::PlayerInputRequest& request) {
  input_.BlockPlayer(request.player(), request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::Press(const wasm::PlayerInputRequest& request) {
  input_.Press(request.player(), request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::WatchInput(const wasm::InputRequest& request) {
  input_.Watch(request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Game::RemapInput(const wasm::RemapInputRequest& request) {
  input_.Remap(request);
  services_.Input();
  return {};
}

}  // namespace modlock::wasm
