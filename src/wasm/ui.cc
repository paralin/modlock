//go:build deps_only

#include "wasm/ui.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace modlock::wasm {
namespace {

// kMaxSlot is the highest player slot.
constexpr int32_t kMaxSlot = 63;

// kMaxNodes bounds one tree, which keeps every redraw cheap.
constexpr size_t kMaxNodes = 512;

// kMaxId and kMaxText bound a node's id and its text, in bytes.
constexpr size_t kMaxId = 64;
constexpr size_t kMaxText = 1024;

// kMaxData bounds the data a layout reads, in bytes. A layout that holds more,
// such as a whole inventory, splits it across several layout nodes.
constexpr size_t kMaxData = 8192;

// kMaxName bounds a token, a variable's name and a dropdown's option, in
// bytes, and kMaxVariables and kMaxOptions bound how many a node holds.
constexpr size_t kMaxName = 128;
constexpr int kMaxVariables = 16;
constexpr int kMaxOptions = 64;

// kMaxInput bounds the text a player sends an entry, in bytes, so its console
// line stays short of the engine's limit; kEntryLength is an entry's limit
// in characters when the mod sets none.
constexpr size_t kMaxInput = 240;
constexpr uint32_t kEntryLength = 100;

// kImageRoots lists where an image may come from: the game's own content.
constexpr std::string_view kImageRoots[] = {"file://{images}/", "s2r://panorama/images/"};

// IsLayoutPath reports whether path names a layout inside the mod's addon: one or
// more segments of letters, digits, '_', '-' and '.', none of them "..", ending
// in ".xml". The renderer joins it to the addon's layout directory, so a path
// that could leave that directory never reaches it.
bool IsLayoutPath(std::string_view path) {
  if (!path.ends_with(".xml")) {
    return false;
  }
  for (auto part : std::views::split(path, '/')) {
    std::string_view segment(part.begin(), part.end());
    if (segment.empty() || segment == "..") {
      return false;
    }
    for (char c : segment) {
      bool allowed =
          std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
      if (!allowed) {
        return false;
      }
    }
  }
  return true;
}

// IsWord reports whether text is one or more letters, digits, '_', and the
// characters in extra.
bool IsWord(std::string_view text, std::string_view extra) {
  return !text.empty() && text.size() <= kMaxName && std::ranges::all_of(text, [&](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || extra.contains(c);
  });
}

// CheckLabel returns why a node's localized text cannot be drawn: a token is
// '#' then a word that may hold '-' and '.', and its variables are named by
// words and bounded.
std::expected<void, std::string> CheckLabel(const ui::Node& node) {
  if (!node.token().empty() &&
      !(node.token().starts_with('#') && IsWord(node.token().substr(1), "-."))) {
    return std::unexpected("node " + node.id() + ": a token is '#' then letters and digits");
  }
  if (node.variables_size() > kMaxVariables) {
    return std::unexpected("node " + node.id() + ": too many variables");
  }
  for (const auto& [name, value] : node.variables()) {
    if (!IsWord(name, "") || value.size() > kMaxText) {
      return std::unexpected("node " + node.id() + ": a variable is a word and short text");
    }
  }
  return {};
}

// CheckInput returns why an input cannot be drawn: it needs a control, and
// an entry's placeholder and a dropdown's options are bounded.
std::expected<void, std::string> CheckInput(const ui::Node& node) {
  // An input names its control.
  if (node.kind() != ui::KIND_INPUT) {
    return {};
  }
  if (node.input_case() == ui::Node::INPUT_NOT_SET) {
    return std::unexpected("node " + node.id() + ": an input needs a control");
  }

  // Its text fits the limits.
  if (node.text_entry().placeholder().size() > kMaxText) {
    return std::unexpected("node " + node.id() + ": the placeholder is too long");
  }
  if (node.dropdown().options_size() > kMaxOptions) {
    return std::unexpected("node " + node.id() + ": too many options");
  }
  for (const auto& option : node.dropdown().options()) {
    if (option.size() > kMaxName) {
      return std::unexpected("node " + node.id() + ": an option is too long");
    }
  }
  return {};
}

// Check returns why node cannot be drawn, if it cannot.
std::expected<void, std::string> Check(const ui::Node& node) {
  // Each field must fit its limit.
  if (node.id().size() > kMaxId) {
    return std::unexpected("a node id is too long");
  }
  if (node.text().size() > kMaxText) {
    return std::unexpected("node " + node.id() + ": text is too long");
  }
  if (node.children_size() > static_cast<int>(kMaxNodes)) {
    return std::unexpected("node " + node.id() + ": too many children");
  }
  if (node.data().size() > kMaxData) {
    return std::unexpected("node " + node.id() + ": data is too long");
  }
  if (node.kind() == ui::KIND_LAYOUT && !IsLayoutPath(node.layout())) {
    return std::unexpected("node " + node.id() +
                           ": a layout is a path ending in .xml in the mod's addon");
  }

  // An image must come from the game's content.
  if (!node.image().empty()) {
    bool allowed = false;
    for (const auto root : kImageRoots) {
      allowed = allowed || node.image().starts_with(root);
    }
    if (!allowed) {
      return std::unexpected("node " + node.id() + ": images come from the game's content");
    }
  }

  // Localized text and inputs have their own limits.
  if (auto checked = CheckLabel(node); !checked) {
    return checked;
  }
  return CheckInput(node);
}

// Base64Digit returns the value of a base64url digit, or -1 for another
// character.
int Base64Digit(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  return c == '-' ? 62 : c == '_' ? 63 : -1;
}

// DecodeBase64Url decodes unpadded base64url, or returns nullopt for text
// that is not.
std::optional<std::string> DecodeBase64Url(std::string_view text) {
  std::string decoded;
  uint32_t bits = 0;
  int count = 0;
  for (char c : text) {
    // Gather six bits per digit and take each whole byte.
    const int digit = Base64Digit(c);
    if (digit < 0) {
      return std::nullopt;
    }
    bits = (bits << 6) | static_cast<uint32_t>(digit);
    count += 6;
    if (count >= 8) {
      count -= 8;
      decoded.push_back(static_cast<char>((bits >> count) & 0xff));
    }
  }
  return decoded;
}

// CharacterSize returns the length in bytes of the UTF-8 character lead
// begins, or zero for a byte that begins none.
size_t CharacterSize(unsigned char lead) {
  if (lead < 0x80) {
    return 1;
  }
  if ((lead >> 5) == 0x6) {
    return 2;
  }
  if ((lead >> 4) == 0xe) {
    return 3;
  }
  return (lead >> 3) == 0x1e ? 4 : 0;
}

// ReadCharacter decodes the UTF-8 character of size bytes at the start of
// text, or returns nullopt for one that is malformed, overlong or a
// surrogate.
std::optional<uint32_t> ReadCharacter(std::string_view text, size_t size) {
  // Gather the lead's bits and six from each continuation byte.
  const uint32_t lead = size == 1 ? 0x7f : 0xff >> (size + 1);
  auto point = static_cast<uint32_t>(static_cast<unsigned char>(text[0])) & lead;
  for (size_t i = 1; i < size; ++i) {
    const auto next = static_cast<unsigned char>(text[i]);
    if ((next & 0xc0) != 0x80) {
      return std::nullopt;
    }
    point = (point << 6) | (next & 0x3f);
  }

  // Refuse a character written longer than it needs, or one UTF-8 forbids.
  constexpr uint32_t kShortest[] = {0, 0, 0x80, 0x800, 0x10000};
  const bool surrogate = point >= 0xd800 && point <= 0xdfff;
  if ((size > 1 && point < kShortest[size]) || point > 0x10ffff || surrogate) {
    return std::nullopt;
  }
  return point;
}

// Clean returns text without its control characters and cut to length
// characters, or nullopt when it is not UTF-8.
std::optional<std::string> Clean(std::string_view text, uint32_t length) {
  std::string cleaned;
  uint32_t characters = 0;
  for (size_t at = 0; at < text.size() && characters < length;) {
    // Read one character.
    const size_t size = CharacterSize(static_cast<unsigned char>(text[at]));
    if (size == 0 || at + size > text.size()) {
      return std::nullopt;
    }
    const auto point = ReadCharacter(text.substr(at), size);
    if (!point) {
      return std::nullopt;
    }

    // Keep it unless it is a control character.
    if (*point >= 0x20 && !(*point >= 0x7f && *point <= 0x9f)) {
      cleaned.append(text.substr(at, size));
      ++characters;
    }
    at += size;
  }
  return cleaned;
}

// Fit clamps value to [min, max], snapped to the nearest step from min when
// step is positive.
float Fit(float value, float min, float max, float step) {
  if (step > 0) {
    value = min + std::round((value - min) / step) * step;
  }
  return std::clamp(value, min, max);
}

}  // namespace

std::expected<void, std::string> UiTrees::Apply(int32_t slot, const ui::Change& change) {
  // The slot must be a player's; a player who left must reset.
  if (slot < 0 || slot > kMaxSlot) {
    return std::unexpected("no player has slot " + std::to_string(slot));
  }
  if (!change.reset() && left_.contains(slot)) {
    return std::unexpected("the player left; send their whole interface with a reset");
  }
  for (const auto& node : change.set()) {
    if (auto checked = Check(node); !checked) {
      return checked;
    }
  }

  // Apply the change to a copy, so a tree that grows too large stays as it was.
  decltype(trees_)::mapped_type tree;
  if (auto found = trees_.find(slot); found != trees_.end() && !change.reset()) {
    tree = found->second;
  }
  for (const auto& node : change.set()) {
    tree[node.id()] = Keep(node);
  }
  for (const auto& id : change.removed()) {
    tree.erase(id);
  }
  if (tree.size() > kMaxNodes) {
    return std::unexpected("the interface has too many nodes");
  }

  // Store the tree, dropping an empty one.
  left_.erase(slot);
  if (tree.empty()) {
    trees_.erase(slot);
  } else {
    trees_[slot] = std::move(tree);
  }
  return {};
}

bool UiTrees::IsButton(int32_t slot, std::string_view node) const {
  auto tree = trees_.find(slot);
  if (tree == trees_.end()) {
    return false;
  }
  auto found = tree->second.find(node);
  return found != tree->second.end() && found->second.kind == ui::KIND_BUTTON;
}

UiTrees::Shown UiTrees::Keep(const ui::Node& node) {
  // Keep the kind, and an input's control and state.
  Shown shown;
  shown.kind = node.kind();
  if (node.kind() != ui::KIND_INPUT) {
    return shown;
  }
  shown.input = node.input_case();
  shown.disabled = node.disabled();

  // Keep the bounds of its value; a slider without a range takes 0 to 1.
  const auto& slider = node.slider();
  if (std::isfinite(slider.min()) && std::isfinite(slider.max()) && slider.max() > slider.min()) {
    shown.min = slider.min();
    shown.max = slider.max();
  }
  shown.step = std::isfinite(slider.step()) && slider.step() > 0 ? slider.step() : 0;
  shown.length = node.text_entry().max_length() ? node.text_entry().max_length() : kEntryLength;
  shown.options = static_cast<uint32_t>(node.dropdown().options_size());
  return shown;
}

std::expected<ui::UiInputEvent, std::string> UiTrees::Input(int32_t slot, std::string_view node,
                                                            std::string_view value) const {
  // Find the input the player sees, which must take changes.
  const auto name = std::string(node);
  const auto tree = trees_.find(slot);
  if (tree == trees_.end()) {
    return std::unexpected("shows no input " + name);
  }
  const auto found = tree->second.find(node);
  if (found == tree->second.end() || found->second.input == ui::Node::INPUT_NOT_SET) {
    return std::unexpected("shows no input " + name);
  }
  const auto& shown = found->second;
  if (shown.disabled) {
    return std::unexpected("input " + name + " is disabled");
  }

  // Read the value in the kind its control holds.
  ui::UiInputEvent event;
  event.set_player(slot);
  event.set_node(name);
  switch (shown.input) {
    case ui::Node::kTextEntry: {
      const auto decoded = DecodeBase64Url(value);
      const auto text =
          decoded && decoded->size() <= kMaxInput ? Clean(*decoded, shown.length) : std::nullopt;
      if (!text) {
        return std::unexpected("input " + name + " takes short UTF-8 text");
      }
      event.set_text(*text);
      break;
    }
    case ui::Node::kSlider: {
      float number = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
      if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
          !std::isfinite(number)) {
        return std::unexpected("input " + name + " takes a number");
      }
      event.set_number(Fit(number, shown.min, shown.max, shown.step));
      break;
    }
    case ui::Node::kToggle:
      if (value != "0" && value != "1") {
        return std::unexpected("input " + name + " takes 1 or 0");
      }
      event.set_on(value == "1");
      break;
    case ui::Node::kDropdown: {
      uint32_t choice = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), choice);
      if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
          choice >= shown.options) {
        return std::unexpected("input " + name + " has no option " + std::string(value));
      }
      event.set_choice(choice);
      break;
    }
    case ui::Node::INPUT_NOT_SET:
      break;
  }
  return event;
}

bool UiTrees::Drop(int32_t slot) {
  if (trees_.erase(slot) == 0) {
    return false;
  }
  left_.insert(slot);
  return true;
}

std::vector<int32_t> UiTrees::Clear() {
  // Name each slot that held a tree, then drop them all.
  std::vector<int32_t> slots;
  for (const auto& [slot, tree] : trees_) {
    slots.push_back(slot);
  }
  trees_.clear();
  left_.clear();
  return slots;
}

}  // namespace modlock::wasm
