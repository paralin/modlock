#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "src/wasm/ui/ui.pb.h"

namespace modlock::wasm {

// UiTrees follows the interface trees one mod shows its players, one tree per
// player slot. It checks each change against the limits a renderer can draw
// and remembers which nodes are buttons and inputs, so only a press on a
// button the player sees, and only a value an input they see can hold, reach
// the mod. When a player leaves, the next change for
// their slot must reset it, so a mod that missed the departure does not send
// the next player a partial tree. It runs on the engine thread.
class UiTrees {
 public:
  // Apply checks change and applies it to slot's tree. A change that breaks
  // a limit, or that does not reset a departed player's slot, leaves the
  // tree as it was.
  [[nodiscard]] std::expected<void, std::string> Apply(int32_t slot, const ui::Change& change);

  // IsButton reports whether slot's tree holds a button named node.
  bool IsButton(int32_t slot, std::string_view node) const;

  // Input checks value, as a player's game sends it, against the input named
  // node in slot's tree and returns the event the mod hears. A text entry's
  // value is its UTF-8 text in unpadded base64url, a slider's a decimal
  // number, a toggle's "1" or "0" and a dropdown's the option's index. Text
  // loses control characters and what passes the entry's length, and a
  // slider's number is clamped to its range and snapped to its step.
  [[nodiscard]] std::expected<ui::UiInputEvent, std::string> Input(int32_t slot,
                                                                   std::string_view node,
                                                                   std::string_view value) const;

  // Drop forgets slot's tree when its player leaves, and reports whether it
  // held any node.
  bool Drop(int32_t slot);

  // Clear forgets every tree and returns the slots that held a node.
  std::vector<int32_t> Clear();

 private:
  // Shown is what the host keeps of one node: its kind, and for an input
  // whether it is disabled and the bounds of the value it holds. length is a
  // text entry's limit in characters, min, max and step a slider's range,
  // and options a dropdown's count.
  struct Shown {
    ui::Kind kind = ui::KIND_PANEL;
    ui::Node::InputCase input = ui::Node::INPUT_NOT_SET;
    bool disabled = false;
    uint32_t length = 0;
    float min = 0;
    float max = 1;
    float step = 0;
    uint32_t options = 0;
  };

  // Keep returns what the host keeps of node.
  static Shown Keep(const ui::Node& node);

  // trees_ maps each slot to what it keeps of its nodes by id.
  std::map<int32_t, std::map<std::string, Shown, std::less<>>> trees_;
  // left_ holds the slots whose player left since their last reset.
  std::set<int32_t> left_;
};

}  // namespace modlock::wasm
