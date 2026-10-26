//go:build deps_only

#include "wasm/visibility.h"

#include "modlock/gameinterop/connection_tracker.h"

namespace modlock::wasm {

std::expected<uint64_t, std::string> Visibility::Audience(
    const google::protobuf::RepeatedField<int32_t>& players) {
  uint64_t slots = 0;
  for (const auto slot : players) {
    if (!gameinterop::IsPlayerSlot(slot)) {
      return std::unexpected("player slot out of range");
    }
    slots |= uint64_t{1} << slot;
  }
  return slots;
}

void Visibility::Unhide(int32_t slot, uint32_t object) {
  auto hidden = hidden_.find(slot);
  if (hidden == hidden_.end()) {
    return;
  }
  hidden->second.erase(object);
  if (hidden->second.empty()) {
    hidden_.erase(hidden);
  }
}

void Visibility::RemoveObject(uint32_t object, std::optional<uint32_t> entity) {
  // Withhold the entity of an object some players must not see.
  const bool restricted =
      object_audiences_.erase(object) != 0 || std::ranges::any_of(hidden_, [&](const auto& entry) {
        return entry.second.contains(object);
      });
  if (restricted && entity) {
    audiences_[*entity] = 0;
  }

  // Drop it from every hidden set.
  for (auto& [slot, objects] : hidden_) {
    objects.erase(object);
  }
}

void Visibility::Leave(int32_t slot) {
  // Show the slot every object, and take its bit out of every audience.
  hidden_.erase(slot);
  if (!gameinterop::IsPlayerSlot(slot)) {
    return;
  }
  const uint64_t bit = uint64_t{1} << slot;
  for (auto& [entity, slots] : audiences_) {
    slots &= ~bit;
  }
  for (auto& [object, slots] : object_audiences_) {
    slots &= ~bit;
  }
}

}  // namespace modlock::wasm
