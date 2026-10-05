#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "proto/modlock/ui.pb.h"

namespace modlock::wasm {

// UiTrees follows the interface trees one mod shows its players, one tree per
// player slot. It checks each change against the limits a renderer can draw
// and remembers which nodes are buttons, so only a press on a button the
// player sees reaches the mod. When a player leaves, the next change for
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

  // Drop forgets slot's tree when its player leaves, and reports whether it
  // held any node.
  bool Drop(int32_t slot);

  // Clear forgets every tree and returns the slots that held a node.
  std::vector<int32_t> Clear();

 private:
  // trees_ maps each slot to its nodes' kinds by id.
  std::map<int32_t, std::map<std::string, ui::Kind, std::less<>>> trees_;
  // left_ holds the slots whose player left since their last reset.
  std::set<int32_t> left_;
};

}  // namespace modlock::wasm
