#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "src/wasm/ui/ui.pb.h"

namespace modlock::wasm {

// DocumentResult is what one frame settles for a submission: nothing yet, or
// the whole document or why it failed.
using DocumentResult = std::optional<std::expected<std::string, ui::DocumentFailure>>;

// UiDocuments assembles at most one submission per authenticated player,
// across all of the host's mods. It never retains a callback or a player id
// claimed by the client.
class UiDocuments {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr size_t kLimit = 262144;

  // kDeadline and kBytesPerSecond bound a transfer's life: a fixed start-up
  // allowance and the renderer's pace of four 180-byte frames every 50 ms,
  // so a document of any allowed size has its sending time and the same
  // margin.
  static constexpr auto kDeadline = std::chrono::seconds(20);
  static constexpr size_t kBytesPerSecond = 14400;

  // Take processes a frame for the currently authorized control. A frame that
  // does not belong to the control's current submission settles nothing. The
  // document is returned only after an explicit end with exactly the
  // declared UTF-8 bytes; any other end returns its failure once.
  DocumentResult Take(int32_t slot, uint64_t control, uint64_t& last, std::string_view frame,
                      Clock::time_point now);

  // Cancel discards the in-flight bytes if they belong to this control.
  void Cancel(int32_t slot, uint64_t control);

  // Expire ends the unfinished submissions of controls, keyed by slot, that
  // passed their deadline, and returns their slots. Each mod passes only its
  // own controls, so it hears exactly its own expiries.
  std::vector<int32_t> Expire(Clock::time_point now, const std::map<int32_t, uint64_t>& controls);

 private:
  struct Transfer {
    uint64_t control;
    uint64_t serial;
    size_t total;
    Clock::time_point until;
    std::string bytes;
  };

  std::map<int32_t, Transfer> transfers_;
};

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
  [[nodiscard]] std::expected<void, std::string> Apply(
      int32_t slot, const ui::Change& change, std::optional<ui::Change>* stamped = nullptr);

  // DocumentControl is the host-issued lifetime of the slot's enabled form.
  uint64_t DocumentControl(int32_t slot) const;

  // DocumentControls lists the form lifetimes to cancel when this mod stops.
  const std::map<int32_t, uint64_t>& DocumentControls() const { return document_controls_; }

  // Document takes one renderer frame only for the current reachable form,
  // and returns the event that ends its submission.
  std::optional<ui::UiDocumentEvent> Document(int32_t slot, std::string_view node,
                                              std::string_view frame, UiDocuments& documents,
                                              UiDocuments::Clock::time_point now);

  // ExpireDocuments ends this mod's submissions that passed their deadline
  // and returns an expiry event for each.
  std::vector<ui::UiDocumentEvent> ExpireDocuments(UiDocuments& documents,
                                                   UiDocuments::Clock::time_point now);

  // IsButton reports whether slot's tree holds a button named node.
  bool IsButton(int32_t slot, std::string_view node) const;

  // GameAction reads a game action's phase as a player's game sends it,
  // "<action> <phase>" with each as its enum's number, and returns the event
  // the mod hears from slot while slot's root follows that action.
  [[nodiscard]] std::expected<ui::UiActionEvent, std::string> GameAction(
      int32_t slot, std::string_view line) const;

  // Overlay reads the stock overlay a player's game reports, its enum's
  // number, and returns the event the mod hears from slot while slot's root
  // follows overlays.
  [[nodiscard]] std::expected<ui::UiOverlayEvent, std::string> Overlay(int32_t slot,
                                                                       std::string_view line) const;

  // Placement reads where a player's game placed the mod's interface, as the
  // game sends it: the screen's width and height, the HUD's x, y, width and
  // height, and then each measured node's name, x, y, width and height, all
  // in layout units and separated by single spaces. A report holds only the
  // boxes that changed, so Placement merges it into what slot reported
  // before and returns the whole placement the mod hears, while slot's root
  // follows placement. It keeps a box only for a name slot's tree measures.
  [[nodiscard]] std::expected<ui::UiPlacementEvent, std::string> Placement(int32_t slot,
                                                                           std::string_view line);

  // kMaxPlacement is the largest distance, in layout units, a report holds.
  static constexpr int32_t kMaxPlacement = 1 << 20;

  // Input checks value, as a player's game sends it, against the input named
  // node in slot's tree and returns the event the mod hears. A text entry's
  // value is its UTF-8 text in unpadded base64url, a slider's a decimal
  // number, a toggle's "1" or "0" and a dropdown's the option's index. Text
  // loses control characters and what passes the entry's length, and a
  // slider's number is clamped to its range and snapped to its step.
  // A slider that opted in to wheel input accepts "wheel:direction:fraction",
  // with direction 1 or -1 and a finite fraction clamped from 0 to 1.
  [[nodiscard]] std::expected<ui::UiInputEvent, std::string> Input(int32_t slot,
                                                                   std::string_view node,
                                                                   std::string_view value) const;

  // Action checks a press a layout's script sent, as a player's game sends
  // it: "<node> <action>" and then the payload's UTF-8 in unpadded
  // base64url, which an empty payload leaves off. It returns the event the
  // mod hears from slot, the player the engine names, while slot's tree shows
  // the layout node and the player keeps under kActionsPerSecond presses a
  // second after a burst of kActionBurst. A name, a payload or a pace past
  // those is refused, and so is every press once the tree is gone.
  [[nodiscard]] std::expected<ui::UiPressEvent, std::string> Action(
      int32_t slot, std::string_view line, std::chrono::steady_clock::time_point now);

  // kActionPayload is the most bytes of payload a layout's press carries.
  static constexpr size_t kActionPayload = 180;

  // kActionBurst and kActionsPerSecond bound the presses one player's
  // layouts send: a burst, then a steady pace.
  static constexpr double kActionBurst = 8;
  static constexpr double kActionsPerSecond = 4;

  // Drop forgets slot's tree when its player leaves, and reports whether it
  // held any node.
  bool Drop(int32_t slot);

  // Clear forgets every tree and returns the slots that held a node.
  std::vector<int32_t> Clear();

 private:
  // Shown is what the host keeps of one node: its kind, and for an input
  // whether it is disabled and the bounds of the value it holds. length is a
  // text entry's limit in characters, min, max and step a slider's range,
  // options a dropdown's count, and wheel a slider's wheel opt-in.
  struct Shown {
    ui::Kind kind = ui::KIND_PANEL;
    ui::Node::InputCase input = ui::Node::INPUT_NOT_SET;
    bool disabled = false;
    bool wheel = false;
    uint64_t document_control = 0;
    uint64_t last_submission = 0;
    std::shared_ptr<const std::vector<std::string>> children;
    uint32_t length = 0;
    float min = 0;
    float max = 1;
    float step = 0;
    uint32_t options = 0;
    // ability_bar is the root's ask for presses on the game's ability bar.
    bool ability_bar = false;
    // game_actions is the root's list of game actions it follows.
    std::vector<ui::GameAction> game_actions;
    // follows_overlay is the root's ask for each change of stock overlay.
    bool follows_overlay = false;
    // follows_placement is the root's ask for each change of its placement.
    bool follows_placement = false;
    // measure is the name the node's bounds are reported by, or empty.
    std::string measure;
  };

  // Keep returns what the host keeps of node.
  static Shown Keep(const ui::Node& node);

  // trees_ maps each slot to what it keeps of its nodes by id.
  std::map<int32_t, std::map<std::string, Shown, std::less<>>> trees_;
  // left_ holds the slots whose player left since their last reset.
  std::set<int32_t> left_;
  std::map<int32_t, uint64_t> document_controls_;

  // Pace is what one player's layouts have left of their presses: the
  // presses they may still send, as of when.
  struct Pace {
    double presses = kActionBurst;
    std::chrono::steady_clock::time_point when;
  };

  // paces_ maps each slot that pressed through a layout to its pace.
  std::map<int32_t, Pace> paces_;

  // Reported is the box of each measured name one player's game reported.
  // The screen and HUD come whole in every report, so only boxes are kept.
  struct Reported {
    std::map<std::string, ui::Bounds, std::less<>> measured;
  };

  // placements_ maps each slot whose game reported a placement to its boxes.
  std::map<int32_t, Reported> placements_;
};

}  // namespace modlock::wasm
