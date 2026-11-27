//go:build deps_only

#include "wasm/ui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
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

// kMaxHandles bounds a range's handles, so a release's values fit a console
// line.
constexpr int kMaxHandles = 8;

// kMaxInput bounds the text a player sends an entry, in bytes, so its console
// line stays short of the engine's limit; kEntryLength is an entry's limit
// in characters when the mod sets none.
constexpr size_t kMaxInput = 240;

// IsAbilityBarNode reports whether node is the reserved press of a click on
// the game's ability bar: "ability:" and a slot from 0 to 3.
bool IsAbilityBarNode(std::string_view node) {
  constexpr std::string_view kPrefix = "ability:";
  return node.size() == kPrefix.size() + 1 && node.starts_with(kPrefix) && node.back() >= '0' &&
         node.back() <= '3';
}

constexpr uint32_t kEntryLength = 100;

// kMaxClasses bounds a node's classes, and kMaxSheets the root's style sheets.
constexpr int kMaxClasses = 16;
constexpr int kMaxSheets = 16;

// kMaxBindings bounds a layout's bindings.
constexpr int kMaxBindings = 64;

// kMaxTemplate bounds a list's row template, kMaxFields a row's fields, and
// kMaxRowHeight a row's height in layout pixels.
constexpr int kMaxTemplate = 16;
constexpr int kMaxFields = 16;
constexpr float kMaxRowHeight = 1000;

// kImageRoots lists where an image may come from: the game's own content.
constexpr std::string_view kImageRoots[] = {"file://{images}/", "s2r://panorama/images/"};

// IsAddonPath reports whether path names a file of kind extension inside the
// mod's addon: one or more segments of letters, digits, '_', '-' and '.', none
// of them "..", ending in extension. The renderer joins a layout's or a style
// sheet's path to the addon's directory, so a path that could leave that
// directory never reaches it.
bool IsAddonPath(std::string_view path, std::string_view extension) {
  if (!path.ends_with(extension)) {
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

// CheckBindings returns why a node's bindings cannot be drawn: only a layout
// holds them, a bounded number, each naming a different panel by a word that
// may hold '-' and '.', with bounded text.
std::expected<void, std::string> CheckBindings(const ui::Node& node) {
  // Only a layout holds bindings, and a bounded number of them.
  if (node.bindings_size() == 0) {
    return {};
  }
  if (node.kind() != ui::KIND_LAYOUT) {
    return std::unexpected("node " + node.id() + ": only a layout holds bindings");
  }
  if (node.bindings_size() > kMaxBindings) {
    return std::unexpected("node " + node.id() + ": too many bindings");
  }

  // Each names its own panel and holds short text.
  std::unordered_set<std::string_view> panels;
  for (const auto& binding : node.bindings()) {
    if (binding.panel().size() > kMaxId || !IsWord(binding.panel(), "-.")) {
      return std::unexpected("node " + node.id() +
                             ": a binding's panel is at most 64 letters, digits, '_', '-' and '.'");
    }
    if (!panels.insert(binding.panel()).second) {
      return std::unexpected("node " + node.id() + ": two bindings name panel " + binding.panel());
    }
    if (binding.text().size() > kMaxText) {
      return std::unexpected("node " + node.id() + ": binding " + binding.panel() +
                             ": text is too long");
    }
  }
  return {};
}

// CheckInput returns why an input cannot be drawn: it needs a control, an
// entry's placeholder and a dropdown's options are bounded, and a range holds
// one to kMaxHandles handles with distinct names.
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

  // A range's events name its handles, so each name is a distinct word.
  if (!node.has_range()) {
    return {};
  }
  const auto& handles = node.range().handles();
  if (handles.empty() || handles.size() > kMaxHandles) {
    return std::unexpected("node " + node.id() + ": a range holds one to eight handles");
  }
  for (int i = 0; i < handles.size(); ++i) {
    const auto& name = handles[i].name();
    const auto same = [&](const ui::RangeHandle& other) {
      return other.name() == name;
    };
    if (name.size() > kMaxId || !IsWord(name, "-.") ||
        std::any_of(handles.begin(), handles.begin() + i, same)) {
      return std::unexpected("node " + node.id() + ": a range's handles have distinct names");
    }
  }
  return {};
}

// IsContentImage reports whether path is an image in the game's content.
bool IsContentImage(std::string_view path) {
  return std::ranges::any_of(kImageRoots, [&](auto root) { return path.starts_with(root); });
}

// CheckAbilityDisplay returns why a node's ability display cannot be drawn:
// a row per player at most, four abilities a row, and short names, a hero
// that is a word, and pictures from the game's content.
std::expected<void, std::string> CheckAbilityDisplay(const ui::Node& node) {
  // The rows fit the players.
  if (node.ability_display_size() > kMaxSlot + 1) {
    return std::unexpected("node " + node.id() + ": too many ability rows");
  }
  for (const auto& row : node.ability_display()) {
    // Each row names a player as the game's Tab row does.
    if (row.player() > static_cast<uint32_t>(kMaxSlot) || row.name().size() > kMaxName ||
        (!row.hero().empty() && (row.hero().size() > kMaxName || !IsWord(row.hero(), "")))) {
      return std::unexpected("node " + node.id() +
                             ": an ability row names a player's slot, a short name and a hero");
    }

    // Each of its four abilities shows a picture from the game's content.
    if (row.abilities_size() > 4) {
      return std::unexpected("node " + node.id() + ": an ability row holds four abilities");
    }
    for (const auto& shown : row.abilities()) {
      if (!shown.image().empty() && !IsContentImage(shown.image())) {
        return std::unexpected("node " + node.id() + ": images come from the game's content");
      }
    }
  }
  return {};
}

// Placeholder returns the field an image names as "{field}", or an empty
// view when it names none.
std::string_view Placeholder(std::string_view image) {
  if (image.size() < 3 || image.front() != '{' || image.back() != '}') {
    return {};
  }
  const auto field = image.substr(1, image.size() - 2);
  return IsWord(field, "") ? field : std::string_view{};
}

std::expected<void, std::string> Check(const ui::Node& node);

// CheckList returns why a list node cannot be drawn: its row template holds
// at most kMaxTemplate panels, labels and images with distinct ids, one of
// them the row's own panel, whose children name only template nodes, and an
// image the template fills from a field passes as content. Rows travel in
// the change, never in the node, and a row's height is bounded.
std::expected<void, std::string> CheckList(const ui::Node& node) {
  if ((node.kind() == ui::KIND_LIST) != node.has_list()) {
    return std::unexpected("node " + node.id() + ": a list node holds a list, and only it");
  }
  if (!node.has_list()) {
    return {};
  }
  const auto& list = node.list();
  if (list.rows_size() > 0 || list.first() != 0 || list.total() != 0) {
    return std::unexpected("node " + node.id() + ": a list's rows travel in the change's lists");
  }
  if (!list.focus().empty() || list.has_focus_at()) {
    return std::unexpected("node " + node.id() + ": a list's focus is the player's");
  }
  if (list.selected().size() > kMaxId) {
    return std::unexpected("node " + node.id() + ": a list's selected key is at most 64 bytes");
  }
  if (!(list.row_height() >= 1 && list.row_height() <= kMaxRowHeight)) {
    return std::unexpected("node " + node.id() + ": a list's row height is 1 to 1000");
  }
  if (list.row_size() > kMaxTemplate) {
    return std::unexpected("node " + node.id() + ": a list's row holds at most 16 nodes");
  }
  std::set<std::string_view> ids;
  for (const auto& part : list.row()) {
    if (part.kind() != ui::KIND_PANEL && part.kind() != ui::KIND_LABEL &&
        part.kind() != ui::KIND_IMAGE) {
      return std::unexpected("node " + node.id() +
                             ": a list's row holds panels, labels and images");
    }
    if (!ids.insert(part.id()).second) {
      return std::unexpected("node " + node.id() + ": a list's row repeats node " + part.id());
    }
    ui::Node checked = part;
    if (!Placeholder(part.image()).empty()) {
      checked.clear_image();
    }
    if (auto result = Check(checked); !result) {
      return result;
    }
  }
  if (!ids.contains("")) {
    return std::unexpected("node " + node.id() + ": a list's row has a node with an empty id");
  }
  for (const auto& part : list.row()) {
    for (const auto& child : part.children()) {
      if (child.empty() || !ids.contains(child)) {
        return std::unexpected("node " + node.id() + ": a list's row names only its own nodes");
      }
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
  if (!node.id().empty() && !IsWord(node.id(), "-.:")) {
    return std::unexpected("a node id is letters, digits, '_', '-', '.' and ':'");
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
  if (!node.measure().empty() &&
      (node.measure().size() > kMaxId || !IsWord(node.measure(), "-."))) {
    return std::unexpected("node " + node.id() +
                           ": a measure is at most 64 letters, digits, '_', '-' and '.'");
  }
  if (node.kind() == ui::KIND_LAYOUT && !IsAddonPath(node.layout(), ".xml")) {
    return std::unexpected("node " + node.id() +
                           ": a layout is a path ending in .xml in the mod's addon");
  }

  // A class is a word that may hold '-', and a sheet a path in the addon.
  if (node.classes_size() > kMaxClasses) {
    return std::unexpected("node " + node.id() + ": too many classes");
  }
  for (const auto& name : node.classes()) {
    if (!IsWord(name, "-")) {
      return std::unexpected("node " + node.id() + ": a class is letters, digits, '_' and '-'");
    }
  }
  if (node.sheets_size() > kMaxSheets) {
    return std::unexpected("node " + node.id() + ": too many style sheets");
  }
  for (const auto& sheet : node.sheets()) {
    if (!IsAddonPath(sheet, ".css")) {
      return std::unexpected("node " + node.id() +
                             ": a style sheet is a path ending in .css in the mod's addon");
    }
  }

  // An image must come from the game's content.
  if (!node.image().empty() && !IsContentImage(node.image())) {
    return std::unexpected("node " + node.id() + ": images come from the game's content");
  }

  if ((node.kind() == ui::KIND_MENU) != node.has_menu() ||
      (node.has_menu() && (node.id().empty() || node.menu().open_label().size() > kMaxName ||
                           !ui::MenuMode_IsValid(node.menu().mode())))) {
    return std::unexpected("node " + node.id() + ": invalid menu");
  }
  if ((node.kind() == ui::KIND_DOCUMENT_INPUT) != node.has_document_input() ||
      (node.has_document_input() &&
       (node.id().empty() || node.document_input().placeholder().size() > kMaxText ||
        node.document_input().submit_label().size() > kMaxName || !node.children().empty()))) {
    return std::unexpected("node " + node.id() + ": invalid document input");
  }
  if (node.kind() == ui::KIND_DOCUMENT_OUTPUT
          ? node.id().empty() || node.document_text().size() > UiDocuments::kLimit ||
                !node.children().empty() || node.document_text().contains('\0')
          : !node.document_text().empty()) {
    return std::unexpected("node " + node.id() + ": invalid document output");
  }

  // Localized text, inputs and the ability display have their own limits.
  if (auto checked = CheckLabel(node); !checked) {
    return checked;
  }
  if (auto checked = CheckAbilityDisplay(node); !checked) {
    return checked;
  }
  if (auto checked = CheckBindings(node); !checked) {
    return checked;
  }
  if (auto checked = CheckList(node); !checked) {
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

// IsActionName reports whether name can name a layout's press: one to 64
// letters, digits, '_', '-' and '.'.
bool IsActionName(std::string_view name) {
  return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
  });
}

// IsUtf8 reports whether text is well-formed UTF-8.
bool IsUtf8(std::string_view text) {
  while (!text.empty()) {
    const auto size = CharacterSize(static_cast<unsigned char>(text[0]));
    if (size == 0 || size > text.size() || !ReadCharacter(text, size)) {
      return false;
    }
    text.remove_prefix(size);
  }
  return true;
}

// TakeField reads one colon-delimited field and advances past its separator.
std::string_view TakeField(std::string_view& line) {
  const auto end = line.find(':');
  const auto field = line.substr(0, end);
  line.remove_prefix(end == std::string_view::npos ? line.size() : end + 1);
  return field;
}

// Decimal reads a whole unsigned decimal field without overflow.
std::optional<uint64_t> Decimal(std::string_view field) {
  if (field.empty()) {
    return std::nullopt;
  }
  uint64_t value = 0;
  const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
  if (error != std::errc{} || end != field.data() + field.size()) {
    return std::nullopt;
  }
  return value;
}

// NextDocumentControl gives each new form a host-unique identity, even if
// another player or another mod uses the same node id.
uint64_t NextDocumentControl() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

DocumentResult UiDocuments::Take(int32_t slot, uint64_t control, uint64_t& last,
                                 std::string_view frame, Clock::time_point now) {
  // Match the control and submission before changing any pending transfer.
  if (frame.size() > 320 || Decimal(TakeField(frame)) != control) {
    return std::nullopt;
  }
  const auto serial = Decimal(TakeField(frame));
  if (!serial || *serial == 0) {
    return std::nullopt;
  }
  auto found = transfers_.find(slot);

  // A submission starts only while the player has none in flight. One past
  // the limit fails at once, before any of its bytes arrive.
  if (frame.starts_with("b:")) {
    const auto total = Decimal(frame.substr(2));
    if (!total || *serial <= last || found != transfers_.end()) {
      return std::nullopt;
    }
    last = *serial;
    if (*total > kLimit) {
      return std::unexpected(ui::DOCUMENT_FAILURE_TOO_LARGE);
    }
    const auto until = now + kDeadline + std::chrono::milliseconds(*total * 1000 / kBytesPerSecond);
    auto [started, inserted] = transfers_.try_emplace(
        slot, Transfer{control, *serial, static_cast<size_t>(*total), until, {}});
    static_cast<void>(inserted);
    started->second.bytes.reserve(static_cast<size_t>(*total));
    return std::nullopt;
  }
  if (found == transfers_.end() || found->second.control != control ||
      found->second.serial != *serial) {
    return std::nullopt;
  }

  // A frame after the deadline ends the transfer as expired, even before the
  // tick's expiry reaches it.
  auto& transfer = found->second;
  if (now >= transfer.until) {
    transfers_.erase(found);
    return std::unexpected(ui::DOCUMENT_FAILURE_EXPIRED);
  }
  if (frame.starts_with("p:")) {
    // Read the chunk from a copy, so a malformed chunk never leaves frame
    // looking like an end or a cancel below.
    auto chunk = frame.substr(2);
    const auto offset = Decimal(TakeField(chunk));
    constexpr size_t kChunk = 180;
    const auto count = std::min(kChunk, transfer.total - transfer.bytes.size());
    if (offset == transfer.bytes.size() && count != 0 && chunk.size() == (count * 4 + 2) / 3) {
      if (auto decoded = DecodeBase64Url(chunk); decoded && decoded->size() == count) {
        transfer.bytes.append(*decoded);
        return std::nullopt;
      }
    }
  }

  // Any other frame ends this transfer. Remove it before calling the mod so
  // replayed completion or changes made inside its callback cannot deliver twice.
  auto bytes = std::move(transfer.bytes);
  const auto complete = bytes.size() == transfer.total;
  transfers_.erase(found);
  if (frame == "x") {
    return std::unexpected(ui::DOCUMENT_FAILURE_CANCELLED);
  }
  if (frame == "e" && complete && !bytes.contains('\0') && IsUtf8(bytes)) {
    return bytes;
  }
  return std::unexpected(ui::DOCUMENT_FAILURE_MALFORMED);
}

void UiDocuments::Cancel(int32_t slot, uint64_t control) {
  const auto found = transfers_.find(slot);
  if (found != transfers_.end() && found->second.control == control) {
    transfers_.erase(found);
  }
}

std::vector<int32_t> UiDocuments::Expire(Clock::time_point now,
                                         const std::map<int32_t, uint64_t>& controls) {
  // A player has at most one transfer, so the walk is short.
  std::vector<int32_t> expired;
  for (auto each = transfers_.begin(); each != transfers_.end();) {
    const auto owner = controls.find(each->first);
    if (now >= each->second.until && owner != controls.end() &&
        owner->second == each->second.control) {
      expired.push_back(each->first);
      each = transfers_.erase(each);
    } else {
      ++each;
    }
  }
  return expired;
}

std::expected<void, std::string> UiTrees::Apply(int32_t slot, const ui::Change& change,
                                                std::optional<ui::Change>* stamped) {
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
    auto kept = Keep(node);
    if (node.kind() == ui::KIND_DOCUMENT_INPUT && !node.disabled()) {
      kept.document_control = NextDocumentControl();
    }

    // A list keeps its rows through a change to its node.
    auto& slot_node = tree[node.id()];
    if (kept.kind == ui::KIND_LIST && slot_node.kind == ui::KIND_LIST) {
      kept.rows = slot_node.rows;
    }
    slot_node = std::move(kept);
  }
  for (const auto& id : change.removed()) {
    tree.erase(id);
  }
  if (tree.size() > kMaxNodes) {
    return std::unexpected("the interface has too many nodes");
  }
  auto lists = CheckLists(tree, change);
  if (!lists) {
    return std::unexpected(lists.error());
  }

  // A player gets one form and one export per tree. Only an input reachable
  // from the root may receive a document, even if a native caller leaves an
  // orphan node in its delta. Local menu visibility is the renderer's job.
  const std::string* form = nullptr;
  bool output = false;
  for (const auto& [id, node] : tree) {
    if (node.kind == ui::KIND_DOCUMENT_INPUT) {
      if (form) {
        return std::unexpected("the interface has more than one document input");
      }
      form = &id;
    }
    if (node.kind == ui::KIND_DOCUMENT_OUTPUT && std::exchange(output, true)) {
      return std::unexpected("the interface has more than one document output");
    }
  }
  if (form) {
    std::vector<std::string_view> todo = {""};
    std::set<std::string_view> seen;
    bool reached = false;
    while (!todo.empty()) {
      const auto id = todo.back();
      todo.pop_back();
      if (id == *form) {
        reached = true;
        break;
      }
      if (!seen.insert(id).second) {
        continue;
      }
      const auto found = tree.find(id);
      if (found == tree.end() || !found->second.children || found->second.kind == ui::KIND_LAYOUT) {
        continue;
      }
      for (const auto& child : *found->second.children) {
        todo.emplace_back(child);
      }
    }
    if (!reached) {
      return std::unexpected("the document input is not reachable from the root");
    }
  }
  if (stamped && std::ranges::any_of(change.set(), [](const ui::Node& node) {
        return node.kind() == ui::KIND_DOCUMENT_INPUT;
      })) {
    auto& outgoing = stamped->emplace(change);
    for (auto& node : *outgoing.mutable_set()) {
      if (node.kind() != ui::KIND_DOCUMENT_INPUT) {
        continue;
      }
      const auto found = tree.find(node.id());
      node.mutable_document_input()->set_control(
          found == tree.end() ? 0 : found->second.document_control);
    }
  }
  const auto control = form ? tree.find(*form)->second.document_control : 0;
  if (control) {
    document_controls_[slot] = control;
  } else {
    document_controls_.erase(slot);
  }

  // Apply the rows, then store the tree, dropping an empty one. The rows
  // change in place: the tree they share is the one this replaces.
  Commit(*lists);
  left_.erase(slot);
  if (tree.empty()) {
    trees_.erase(slot);
  } else {
    trees_[slot] = std::move(tree);
  }
  return {};
}

std::expected<std::vector<UiTrees::Pending>, std::string> UiTrees::CheckLists(
    std::map<std::string, Shown, std::less<>>& tree, const ui::Change& change) {
  // Each change names a list node of the tree, once.
  std::vector<Pending> lists;
  std::set<std::string_view> named;
  for (const auto& rows : change.lists()) {
    const auto found = tree.find(rows.node());
    if (found == tree.end() || found->second.kind != ui::KIND_LIST) {
      return std::unexpected("node " + rows.node() + " is not a list");
    }
    if (!named.insert(rows.node()).second) {
      return std::unexpected("the change names list " + rows.node() + " twice");
    }
    auto& pending = lists.emplace_back(Pending{.list = &found->second, .reset = rows.reset()});
    const auto& fields = found->second.image_fields;

    // Size each row it sets, which has a short key and few short fields, and
    // whose image fields pass as content.
    for (const auto& key : rows.removed()) {
      pending.touched[key] = std::nullopt;
    }
    for (const auto& row : rows.set()) {
      if (row.key().empty() || row.key().size() > kMaxId) {
        return std::unexpected("list " + rows.node() + ": a row's key is 1 to 64 bytes");
      }
      if (row.fields_size() > kMaxFields) {
        return std::unexpected("list " + rows.node() + ": a row holds at most 16 fields");
      }
      for (const auto& [name, value] : row.fields()) {
        if (!IsWord(name, "") || value.size() > kMaxText) {
          return std::unexpected("list " + rows.node() +
                                 ": a row's field is a word and short text");
        }
        if (std::ranges::contains(fields, name) && !value.empty() && !IsContentImage(value)) {
          return std::unexpected("list " + rows.node() + ": images come from the game's content");
        }
      }
      pending.touched[row.key()] = row.ByteSizeLong();
    }

    // Count what the list holds once the change applies.
    const Rows* base = rows.reset() || !found->second.rows ? nullptr : found->second.rows.get();
    pending.count = base ? base->sizes.size() : 0;
    pending.bytes = base ? base->bytes : 0;
    for (const auto& [key, size] : pending.touched) {
      if (base) {
        if (const auto old = base->sizes.find(key); old != base->sizes.end()) {
          pending.count--;
          pending.bytes -= old->second;
        }
      }
      if (size) {
        pending.count++;
        pending.bytes += *size;
      }
    }
    if (pending.count > kMaxRows) {
      return std::unexpected("list " + rows.node() + " has too many rows");
    }
  }

  // Bound every list of the tree together, a reset dropping a list's rows.
  size_t bytes = 0;
  for (const auto& [id, shown] : tree) {
    const bool changed = named.contains(id);
    if (shown.kind == ui::KIND_LIST && shown.rows && !changed) {
      bytes += shown.rows->bytes;
    }
  }
  for (const auto& pending : lists) {
    bytes += pending.bytes;
  }
  if (bytes > kMaxListBytes) {
    return std::unexpected("the interface's lists hold too many bytes");
  }
  return lists;
}

void UiTrees::Commit(std::vector<Pending>& lists) {
  for (auto& pending : lists) {
    // A reset, or a list without rows, starts from none.
    auto& rows = pending.list->rows;
    if (pending.reset || !rows) {
      rows = std::make_shared<Rows>();
    }
    for (const auto& [key, size] : pending.touched) {
      if (size) {
        rows->sizes.insert_or_assign(std::string(key), *size);
      } else if (const auto found = rows->sizes.find(key); found != rows->sizes.end()) {
        rows->sizes.erase(found);
      }
    }
    rows->bytes = pending.bytes;
  }
}

std::expected<ui::ListWindow, std::string> UiTrees::Window(int32_t slot,
                                                           std::string_view line) const {
  // Read the list's id, two decimal numbers and the focused key, if any.
  const auto first_space = line.find(' ');
  const auto second_space =
      first_space == std::string_view::npos ? first_space : line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos) {
    return std::unexpected("a window is a list, its first row and its count");
  }
  const auto third_space = line.find(' ', second_space + 1);
  const auto node = line.substr(0, first_space);
  const auto first = Decimal(line.substr(first_space + 1, second_space - first_space - 1));
  const auto count = Decimal(line.substr(second_space + 1, third_space - second_space - 1));
  if (!first || !count || *first > kMaxRows || *count > kMaxWindow) {
    return std::unexpected("a window starts within the list and holds at most 256 rows");
  }
  std::string focus;
  if (third_space != std::string_view::npos) {
    const auto decoded = DecodeBase64Url(line.substr(third_space + 1));
    if (!decoded || decoded->empty() || decoded->size() > kMaxId) {
      return std::unexpected("a window's focus is a row key of 1 to 64 bytes");
    }
    focus = *decoded;
  }

  // The list must be one the player's tree shows.
  const auto tree = trees_.find(slot);
  if (tree == trees_.end()) {
    return std::unexpected("the player has no interface");
  }
  const auto found = tree->second.find(node);
  if (found == tree->second.end() || found->second.kind != ui::KIND_LIST) {
    return std::unexpected("node " + std::string(node) + " is not a list");
  }
  ui::ListWindow window;
  window.set_node(std::string(node));
  window.set_first(static_cast<uint32_t>(*first));
  window.set_count(static_cast<uint32_t>(*count));
  window.set_focus(std::move(focus));
  return window;
}

uint64_t UiTrees::DocumentControl(int32_t slot) const {
  const auto found = document_controls_.find(slot);
  return found == document_controls_.end() ? 0 : found->second;
}

std::optional<ui::UiDocumentEvent> UiTrees::Document(int32_t slot, std::string_view node,
                                                     std::string_view frame, UiDocuments& documents,
                                                     UiDocuments::Clock::time_point now) {
  // Only the slot's current form takes frames.
  const auto control = DocumentControl(slot);
  if (!control) {
    return std::nullopt;
  }
  auto& tree = trees_.find(slot)->second;
  const auto found = tree.find(node);
  if (found == tree.end() || found->second.document_control != control) {
    return std::nullopt;
  }
  auto result = documents.Take(slot, control, found->second.last_submission, frame, now);
  if (!result) {
    return std::nullopt;
  }

  // The frame ended the submission: report its text or its failure.
  ui::UiDocumentEvent event;
  event.set_player(slot);
  event.set_node(node);
  if (*result) {
    event.mutable_delivered()->set_text(std::move(**result));
  } else {
    event.mutable_failed()->set_reason(result->error());
  }
  return event;
}

std::vector<ui::UiDocumentEvent> UiTrees::ExpireDocuments(UiDocuments& documents,
                                                          UiDocuments::Clock::time_point now) {
  // Name each expired submission by the form that holds its control.
  std::vector<ui::UiDocumentEvent> events;
  for (const auto slot : documents.Expire(now, document_controls_)) {
    auto& event = events.emplace_back();
    event.set_player(slot);
    event.mutable_failed()->set_reason(ui::DOCUMENT_FAILURE_EXPIRED);
    const auto control = document_controls_.at(slot);
    for (const auto& [id, shown] : trees_.at(slot)) {
      if (shown.document_control == control) {
        event.set_node(id);
        break;
      }
    }
  }
  return events;
}

bool UiTrees::IsButton(int32_t slot, std::string_view node) const {
  auto tree = trees_.find(slot);
  if (tree == trees_.end()) {
    return false;
  }
  // A press on the game's ability bar counts while the root asks for them.
  if (IsAbilityBarNode(node)) {
    auto root = tree->second.find("");
    return root != tree->second.end() && root->second.ability_bar;
  }
  auto found = tree->second.find(node);
  return found != tree->second.end() && found->second.kind == ui::KIND_BUTTON;
}

std::expected<ui::UiActionEvent, std::string> UiTrees::GameAction(int32_t slot,
                                                                  std::string_view line) const {
  // Read the action and the phase, each a small decimal number.
  const auto space = line.find(' ');
  if (space == std::string_view::npos) {
    return std::unexpected("sent a game action without a phase");
  }
  int action = 0;
  int phase = 0;
  const auto read = [](std::string_view text, int& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size();
  };
  if (!read(line.substr(0, space), action) || !read(line.substr(space + 1), phase) ||
      !ui::ActionPhase_IsValid(phase) || phase == ui::ACTION_PHASE_UNKNOWN) {
    return std::unexpected("sent a game action it cannot read");
  }

  // Refuse an action the player's root does not follow.
  const auto tree = trees_.find(slot);
  if (tree != trees_.end()) {
    const auto root = tree->second.find("");
    if (root != tree->second.end() &&
        std::ranges::find(root->second.game_actions, action) != root->second.game_actions.end()) {
      ui::UiActionEvent event;
      event.set_player(slot);
      event.set_action(static_cast<ui::GameAction>(action));
      event.set_phase(static_cast<ui::ActionPhase>(phase));
      return event;
    }
  }
  return std::unexpected("follows no game action " + std::to_string(action));
}

std::expected<ui::UiOverlayEvent, std::string> UiTrees::Overlay(int32_t slot,
                                                                std::string_view line) const {
  // Read the overlay, a small decimal number.
  int overlay = 0;
  const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), overlay);
  if (error != std::errc() || end != line.data() + line.size() || !ui::Overlay_IsValid(overlay)) {
    return std::unexpected("sent an overlay it cannot read");
  }

  // Refuse an overlay for a root that does not follow them.
  const auto tree = trees_.find(slot);
  if (tree != trees_.end()) {
    const auto root = tree->second.find("");
    if (root != tree->second.end() && root->second.follows_overlay) {
      ui::UiOverlayEvent event;
      event.set_player(slot);
      event.set_overlay(static_cast<ui::Overlay>(overlay));
      return event;
    }
  }
  return std::unexpected("follows no overlay");
}

std::expected<ui::UiPlacementEvent, std::string> UiTrees::Placement(int32_t slot,
                                                                    std::string_view line) {
  // Refuse a placement for a root that does not follow it.
  const auto tree = trees_.find(slot);
  if (tree == trees_.end()) {
    return std::unexpected("follows no placement");
  }
  const auto& nodes = tree->second;
  if (const auto root = nodes.find(""); root == nodes.end() || !root->second.follows_placement) {
    return std::unexpected("follows no placement");
  }

  // Split the line into words, then read the screen's size and the HUD's box.
  std::vector<std::string_view> words;
  for (const auto word : std::views::split(line, ' ')) {
    words.emplace_back(word.begin(), word.end());
  }
  const auto number = [](std::string_view text, int32_t& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size() && value >= -kMaxPlacement &&
           value <= kMaxPlacement;
  };
  const auto bounds = [&](size_t at, ui::Bounds& box) {
    int32_t x = 0, y = 0, width = 0, height = 0;
    if (!number(words[at], x) || !number(words[at + 1], y) || !number(words[at + 2], width) ||
        !number(words[at + 3], height) || width < 0 || height < 0) {
      return false;
    }
    box.set_x(x);
    box.set_y(y);
    box.set_width(width);
    box.set_height(height);
    return true;
  };
  ui::Bounds screen;
  ui::Bounds hud;
  int32_t width = 0, height = 0;
  if (words.size() < 6 || (words.size() - 6) % 5 != 0 || !number(words[0], width) ||
      !number(words[1], height) || width < 0 || height < 0 || !bounds(2, hud)) {
    return std::unexpected("sent a placement it cannot read");
  }
  screen.set_width(width);
  screen.set_height(height);

  // Read each measured node's box, keeping only the names the tree measures:
  // the renderer may report a name from the tree it drew a frame ago.
  std::set<std::string_view> names;
  for (const auto& [id, node] : nodes) {
    if (!node.measure.empty()) {
      names.insert(node.measure);
    }
  }
  auto& layout = placements_[slot];
  for (size_t at = 6; at < words.size(); at += 5) {
    ui::Bounds box;
    if (!bounds(at + 1, box)) {
      return std::unexpected("sent a placement it cannot read");
    }
    if (names.contains(words[at])) {
      layout.measured[std::string(words[at])] = box;
    }
  }

  // The event holds the whole placement: the newest screen and HUD, and each
  // box the renderer reported for a name the tree still measures.
  std::erase_if(layout.measured, [&](const auto& each) { return !names.contains(each.first); });
  ui::UiPlacementEvent event;
  event.set_player(slot);
  *event.mutable_screen() = screen;
  *event.mutable_hud() = hud;
  for (const auto& [name, box] : layout.measured) {
    auto* each = event.add_measured();
    each->set_name(name);
    *each->mutable_bounds() = box;
  }
  return event;
}

UiTrees::Shown UiTrees::Keep(const ui::Node& node) {
  // Keep the kind, and an input's control and state.
  Shown shown;
  shown.kind = node.kind();
  if (!node.children().empty()) {
    shown.children = std::make_shared<const std::vector<std::string>>(node.children().begin(),
                                                                      node.children().end());
  }
  shown.ability_bar = node.id().empty() && node.ability_bar_presses();
  if (node.id().empty()) {
    for (const auto action : node.game_actions()) {
      shown.game_actions.push_back(static_cast<ui::GameAction>(action));
    }
    shown.follows_overlay = node.follows_overlay();
    shown.follows_placement = node.follows_placement();
  }
  shown.measure = node.measure();
  for (const auto& part : node.list().row()) {
    if (const auto field = Placeholder(part.image()); !field.empty()) {
      shown.image_fields.emplace_back(field);
    }
  }
  shown.bound = node.bindings_size() > 0;
  for (const auto& binding : node.bindings()) {
    if (binding.press()) {
      shown.presses.push_back(binding.panel());
    }
  }
  if (node.kind() != ui::KIND_INPUT) {
    return shown;
  }
  shown.input = node.input_case();
  shown.disabled = node.disabled();

  // Keep the bounds of its value; a slider or a range without them takes 0
  // to 1. A range also keeps its handles' names.
  const auto& slider = node.slider();
  shown.wheel = node.has_slider() && slider.wheel();
  const auto& range = node.range();
  const auto min = node.has_range() ? range.min() : slider.min();
  const auto max = node.has_range() ? range.max() : slider.max();
  const auto step = node.has_range() ? range.step() : slider.step();
  if (std::isfinite(min) && std::isfinite(max) && max > min) {
    shown.min = min;
    shown.max = max;
  }
  shown.step = std::isfinite(step) && step > 0 ? step : 0;
  for (const auto& handle : range.handles()) {
    shown.handles.push_back(handle.name());
  }
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
  if (found != tree->second.end() && found->second.kind == ui::KIND_LIST) {
    return ChooseRow(slot, name, found->second, value);
  }
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
  if (value.starts_with("wheel:")) {
    // Wheel input reaches only a visible, enabled slider that opted in.
    if (!shown.wheel) {
      return std::unexpected("input " + name + " does not take wheel events");
    }
    value.remove_prefix(6);
    const auto separator = value.find(':');
    float delta = 0;
    float fraction = 0;
    if (separator == std::string_view::npos) {
      return std::unexpected("input " + name + " takes a wheel step and pointer fraction");
    }

    // Refuse malformed and nonfinite numbers, and bound the pointer anchor.
    const auto step = std::from_chars(value.data(), value.data() + separator, delta);
    const auto anchor =
        std::from_chars(value.data() + separator + 1, value.data() + value.size(), fraction);
    if (step.ec != std::errc() || step.ptr != value.data() + separator ||
        anchor.ec != std::errc() || anchor.ptr != value.data() + value.size() ||
        (delta != 1 && delta != -1) || !std::isfinite(fraction)) {
      return std::unexpected("input " + name + " takes a wheel step and pointer fraction");
    }
    event.mutable_wheel()->set_delta(delta);
    event.mutable_wheel()->set_fraction(std::clamp(fraction, 0.0F, 1.0F));
    return event;
  }
  if (shown.input == ui::Node::kRange) {
    auto range = ReadRange(shown, value);
    if (!range) {
      return std::unexpected("input " + name + " " + range.error());
    }
    *event.mutable_range() = std::move(*range);
    return event;
  }
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
    case ui::Node::kRange:
    case ui::Node::INPUT_NOT_SET:
      break;
  }
  return event;
}

std::expected<ui::UiInputEvent, std::string> UiTrees::ChooseRow(int32_t slot,
                                                                const std::string& name,
                                                                const Shown& shown,
                                                                std::string_view value) {
  // Read "row:<key>", the key in base64url, and refuse a row the list lacks.
  if (!value.starts_with("row:")) {
    return std::unexpected("list " + name + " takes the row a player chose");
  }
  const auto key = DecodeBase64Url(value.substr(4));
  if (!key || !shown.rows || !shown.rows->sizes.contains(*key)) {
    return std::unexpected("list " + name + " has no such row");
  }
  ui::UiInputEvent event;
  event.set_player(slot);
  event.set_node(name);
  event.set_row(*key);
  return event;
}

std::expected<ui::RangeEvent, std::string> UiTrees::ReadRange(const Shown& shown,
                                                              std::string_view value) {
  // Split "range:<phase>:<handle>" and a release's ":<values>".
  constexpr std::string_view kTakes = "takes a phase, a handle and a release's values";
  if (!value.starts_with("range:")) {
    return std::unexpected(std::string(kTakes));
  }
  value.remove_prefix(6);
  const auto first = value.find(':');
  const auto phase = value.substr(0, first);
  const auto rest = first == std::string_view::npos ? std::string_view() : value.substr(first + 1);
  const auto second = rest.find(':');
  const auto handle = rest.substr(0, second);
  const auto values =
      second == std::string_view::npos ? std::string_view() : rest.substr(second + 1);
  const auto released = phase == "2";
  if (first == std::string_view::npos || (phase != "1" && !released && phase != "3") ||
      released != (second != std::string_view::npos)) {
    return std::unexpected(std::string(kTakes));
  }

  // Refuse a handle the range does not show.
  if (std::ranges::find(shown.handles, handle) == shown.handles.end()) {
    return std::unexpected("has no handle " + std::string(handle));
  }
  ui::RangeEvent event;
  event.set_phase(static_cast<ui::ActionPhase>(phase[0] - '0'));
  event.set_handle(std::string(handle));
  if (!released) {
    return event;
  }

  // A release holds one finite value for each handle, in order once fit to
  // the range and its step.
  auto rest_values = values;
  for (size_t i = 0; i < shown.handles.size(); ++i) {
    const auto comma = rest_values.find(',');
    const auto last = i + 1 == shown.handles.size();
    if (last != (comma == std::string_view::npos)) {
      return std::unexpected("takes one value for each handle");
    }
    const auto text = rest_values.substr(0, comma);
    float number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(number)) {
      return std::unexpected("takes one value for each handle");
    }
    const auto fit = Fit(number, shown.min, shown.max, shown.step);
    if (i > 0 && fit < event.values(static_cast<int>(i) - 1)) {
      return std::unexpected("takes its handles' values in order");
    }
    event.add_values(fit);
    rest_values = last ? std::string_view() : rest_values.substr(comma + 1);
  }
  return event;
}

std::expected<ui::UiPressEvent, std::string> UiTrees::Action(
    int32_t slot, std::string_view line, std::chrono::steady_clock::time_point now) {
  // Split the line into the layout node, the action and the payload.
  const auto first = line.find(' ');
  const auto node = line.substr(0, first);
  const auto rest = first == std::string_view::npos ? std::string_view() : line.substr(first + 1);
  const auto second = rest.find(' ');
  const auto action = rest.substr(0, second);
  const auto encoded =
      second == std::string_view::npos ? std::string_view() : rest.substr(second + 1);

  // Refuse a press from a layout the player's tree does not show.
  auto tree = trees_.find(slot);
  if (tree == trees_.end()) {
    return std::unexpected("shows no layout " + std::string(node));
  }
  auto shown = tree->second.find(node);
  if (shown == tree->second.end() || shown->second.kind != ui::KIND_LAYOUT) {
    return std::unexpected("shows no layout " + std::string(node));
  }
  if (!IsActionName(action)) {
    return std::unexpected("layout " + std::string(node) + " cannot press that name");
  }
  if (shown->second.bound && !std::ranges::contains(shown->second.presses, action)) {
    return std::unexpected("layout " + std::string(node) + " binds no press of " +
                           std::string(action));
  }
  if (encoded.size() > (kActionPayload * 4 + 2) / 3) {
    return std::unexpected("layout " + std::string(node) + " sent a payload over " +
                           std::to_string(kActionPayload) + " bytes");
  }
  const auto payload = DecodeBase64Url(encoded);
  if (!payload || payload->size() > kActionPayload || !IsUtf8(*payload)) {
    return std::unexpected("layout " + std::string(node) +
                           " sent a payload that is not short UTF-8");
  }

  // Refill the player's presses for the time since their last, and spend one.
  auto [pace, fresh] = paces_.try_emplace(slot);
  if (fresh) {
    pace->second.when = now;
  }
  const std::chrono::duration<double> since = now - pace->second.when;
  pace->second.presses = std::min(
      kActionBurst, pace->second.presses + std::max(0.0, since.count()) * kActionsPerSecond);
  pace->second.when = std::max(now, pace->second.when);
  if (pace->second.presses < 1) {
    return std::unexpected("layout " + std::string(node) + " pressed faster than " +
                           std::format("{}", kActionsPerSecond) + " a second");
  }
  pace->second.presses -= 1;

  // The engine named the player, so the press is theirs.
  ui::UiPressEvent event;
  event.set_player(slot);
  event.set_node(std::string(node));
  event.set_action(std::string(action));
  event.set_payload(*payload);
  return event;
}

bool UiTrees::Drop(int32_t slot) {
  paces_.erase(slot);
  placements_.erase(slot);
  document_controls_.erase(slot);
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
  document_controls_.clear();
  left_.clear();
  paces_.clear();
  placements_.clear();
  return slots;
}

}  // namespace modlock::wasm
