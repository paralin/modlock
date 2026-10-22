#include "wasm/ui.h"

#include <cctype>
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
  return {};
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
    tree[node.id()] = node.kind();
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
  return found != tree->second.end() && found->second == ui::KIND_BUTTON;
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
