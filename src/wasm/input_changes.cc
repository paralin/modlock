//go:build deps_only

#include "wasm/input_changes.h"

#include <utility>

#include "wasm/game_internal.h"

namespace modlock::wasm {

void InputChanges::Block(uint64_t buttons) {
  blocked_ = buttons;
}

void InputChanges::BlockPlayer(int32_t slot, uint64_t buttons) {
  if (buttons == 0) {
    slot_blocked_.erase(slot);
  } else {
    slot_blocked_[slot] = buttons;
  }
}

void InputChanges::Press(int32_t slot, uint64_t buttons) {
  presses_[slot] |= buttons;
}

void InputChanges::Watch(uint64_t buttons) {
  watched_ = buttons;
}

void InputChanges::Remap(const input::RemapInputRequest& remap) {
  remap_ = remap;
}

uint64_t InputChanges::Blocked(int32_t slot) const {
  const auto found = slot_blocked_.find(slot);
  return blocked_ | (found == slot_blocked_.end() ? 0 : found->second);
}

void InputChanges::Record(int32_t slot, uint64_t pressed, uint64_t released) {
  if ((pressed | released) & watched_ && changes_.size() < kMaxChanges) {
    changes_.push_back({slot, pressed & watched_, released & watched_});
  }
}

void InputChanges::ApplyPress(gameinterop::AbilityInputHook::Input& input) {
  // Take the pending press for this slot.
  auto press = presses_.find(input.slot);
  if (press == presses_.end()) {
    return;
  }

  // A press is down and newly changed for this one input.
  input.buttons[0] |= press->second;
  input.buttons[1] |= press->second;
  input.buttons[2] &= ~press->second;
  presses_.erase(press);
}

void InputChanges::ApplyRemap(gameinterop::AbilityInputHook::Input& input) const {
  // Move each state's from buttons to the to buttons.
  if (remap_.from() == 0) {
    return;
  }
  const bool held = input.buttons[0] & remap_.from();
  for (auto& state : input.buttons) {
    const bool set = state & remap_.from();
    state &= ~remap_.from();
    if (set) {
      state |= remap_.to();
    }
  }

  // A repeating remap presses the to buttons on every input the from buttons
  // are held.
  if (held && remap_.repeat()) {
    input.buttons[1] |= remap_.to();
  }
}

std::vector<InputChanges::ButtonChange> InputChanges::TakeChanges() {
  return std::exchange(changes_, {});
}

void InputChanges::Leave(int32_t slot) {
  slot_blocked_.erase(slot);
  presses_.erase(slot);
}

void InputChanges::DropPresses() {
  presses_.clear();
}

void InputChanges::Clear() {
  *this = {};
}

}  // namespace modlock::wasm
