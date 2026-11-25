#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "src/wasm/controllers/controllers.pb.h"
#include "src/wasm/input.pb.h"
#include "src/wasm/players/players.pb.h"
#include "wasm/input_changes.h"
#include "wasm/module.h"

namespace modlock::wasm {

class GameServices;

// Input answers a mod's calls on its players' buttons and controllers:
// blocking, pressing, remapping and watching buttons, reading a player's held
// buttons, and watching their controllers. Its changes reach the shared input
// hook as an input source while it lives, and Clear releases them all.
class Input final : public Module {
 public:
  using ButtonChange = InputChanges::ButtonChange;

  // services outlives the input.
  explicit Input(GameServices& services);
  ~Input() override;

  Input(const Input&) = delete;
  Input& operator=(const Input&) = delete;

  std::expected<void, std::string> BlockInput(const wasm::InputRequest& request);
  std::expected<void, std::string> BlockPlayerInput(const wasm::PlayerInputRequest& request);
  std::expected<void, std::string> Press(const wasm::PlayerInputRequest& request);
  std::expected<void, std::string> WatchInput(const wasm::InputRequest& request);
  std::expected<void, std::string> RemapInput(const wasm::RemapInputRequest& request);
  std::expected<wasm::ButtonsResponse, std::string> Buttons(const players::PlayerRequest& request);
  std::expected<void, std::string> WatchControllers(
      const controllers::WatchControllersRequest& request);

  // TakeButtonChanges returns the watched button changes since the last call,
  // in the order players made them.
  std::vector<ButtonChange> TakeButtonChanges();

  // Controllers appends each player whose controllers the mod watches, with
  // the presses and releases since its last call.
  void Controllers(google::protobuf::RepeatedPtrField<controllers::PlayerControllers>* out);

  // WorldEnding drops the pending presses; the blocks stay for the next world.
  void WorldEnding() override;

  // Leave forgets the slot's blocks, presses and controller watch.
  void Leave(int32_t slot) override;

  // Clear releases every block, press, remap and watch.
  void Clear() override;

 private:
  GameServices& services_;
  // changes_ is the mod's blocks, presses, remap and watched buttons.
  InputChanges changes_;
};

}  // namespace modlock::wasm
