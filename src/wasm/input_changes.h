#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "modlock/gameinterop/ability_input_hook.h"
#include "src/wasm/wasm.pb.h"

namespace modlock::wasm {

// InputChanges is one mod's changes to the players' input: the buttons it
// withholds from everyone or from one player, the presses it adds to a
// player's next input, the buttons it remaps, and the buttons whose changes
// it watches. The shared input hook applies every mod's changes to each
// input on the engine thread.
class InputChanges {
 public:
  // ButtonChange is one player's watched buttons that went down or up in one
  // input.
  struct ButtonChange {
    int32_t slot = 0;
    uint64_t pressed = 0;
    uint64_t released = 0;
  };

  // Block withholds buttons from every player, replacing the last block.
  void Block(uint64_t buttons);

  // BlockPlayer withholds buttons from the player in slot, replacing the
  // last block there; zero buttons unblock the player.
  void BlockPlayer(int32_t slot, uint64_t buttons);

  // Press adds buttons to the next input of the player in slot.
  void Press(int32_t slot, uint64_t buttons);

  // Watch selects the buttons whose changes Record queues.
  void Watch(uint64_t buttons);

  // Remap makes remap's from buttons act as its to buttons for every player.
  void Remap(const input::RemapInputRequest& remap);

  // Blocked returns the buttons withheld from the player in slot.
  uint64_t Blocked(int32_t slot) const;

  // Record queues the watched buttons among pressed and released for the
  // mod, up to kMaxChanges it has not received.
  void Record(int32_t slot, uint64_t pressed, uint64_t released);

  // ApplyPress adds the pending press of input's player to input, as down and
  // newly changed, and spends it.
  void ApplyPress(gameinterop::AbilityInputHook::Input& input);

  // ApplyRemap moves the state of the remapped buttons in input.
  void ApplyRemap(gameinterop::AbilityInputHook::Input& input) const;

  // TakeChanges returns the watched button changes since the last call, in
  // the order players made them.
  std::vector<ButtonChange> TakeChanges();

  // Leave forgets the blocks and presses of the player in slot, who left.
  void Leave(int32_t slot);

  // DropPresses forgets every pending press, which a world that ends never
  // delivers.
  void DropPresses();

  // Clear releases every change.
  void Clear();

 private:
  uint64_t blocked_ = 0;
  std::map<int32_t, uint64_t> slot_blocked_;
  std::map<int32_t, uint64_t> presses_;
  input::RemapInputRequest remap_;
  uint64_t watched_ = 0;
  std::vector<ButtonChange> changes_;
};

}  // namespace modlock::wasm
