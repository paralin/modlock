#pragma once

#include <google/protobuf/repeated_field.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "modlock/gameinterop/transmit_hook.h"

namespace modlock::wasm {

// Visibility is one mod's limits on which players receive what it placed: the
// objects hidden from each player, and the objects and entities shown only to
// an audience. Objects are the mod's object identifiers, whose entity changes
// as a text object's words do; entities are handles. The shared transmit hook
// applies every mod's limits to each player's transmit set.
class Visibility {
 public:
  // Audience returns the slot bits of players, refusing a slot out of range.
  static std::expected<uint64_t, std::string> Audience(
      const google::protobuf::RepeatedField<int32_t>& players);

  // Hide withholds object from the player in slot.
  void Hide(int32_t slot, uint32_t object) { hidden_[slot].insert(object); }

  // Unhide sends object to the player in slot again.
  void Unhide(int32_t slot, uint32_t object);

  // ShowObjectOnlyTo sends object only to the players in the slot bits.
  void ShowObjectOnlyTo(uint32_t object, uint64_t slots) { object_audiences_[object] = slots; }

  // ShowObjectToAll sends object to every player again.
  void ShowObjectToAll(uint32_t object) { object_audiences_.erase(object); }

  // ShowEntityOnlyTo sends entity only to the players in the slot bits.
  void ShowEntityOnlyTo(uint32_t entity, uint64_t slots) { audiences_[entity] = slots; }

  // ShowEntityToAll sends entity to every player again.
  void ShowEntityToAll(uint32_t entity) { audiences_.erase(entity); }

  // RemoveObject forgets a removed object. When some players must not see it,
  // its entity stays withheld from everyone until Prune finds it gone, since
  // the engine can retire the entity frames after the removal.
  void RemoveObject(uint32_t object, std::optional<uint32_t> entity);

  // Leave forgets the player in slot, so whoever takes the slot next starts
  // outside every audience and sees every object.
  void Leave(int32_t slot);

  // Prune drops the audience of each entity gone reports gone, so a new
  // entity that takes its index reaches every player.
  template <typename Gone>
  void Prune(Gone gone) {
    std::erase_if(audiences_, [&](const auto& entry) { return gone(entry.first); });
  }

  // Withhold clears from set what its player must not receive. handle_of
  // returns an object's current entity handle, or nothing.
  template <typename HandleOf>
  void Withhold(gameinterop::TransmitSet& set, HandleOf handle_of) const;

  // Clear forgets every limit.
  void Clear() { *this = {}; }

 private:
  // hidden_ maps each player's slot to the objects hidden from them.
  std::map<int32_t, std::set<uint32_t>> hidden_;
  // object_audiences_ and audiences_ map each object and entity shown only to
  // some players to the bits of their slots.
  std::map<uint32_t, uint64_t> object_audiences_;
  std::map<uint32_t, uint64_t> audiences_;
};

template <typename HandleOf>
void Visibility::Withhold(gameinterop::TransmitSet& set, HandleOf handle_of) const {
  // Withhold each object hidden from the set's player.
  if (const auto hidden = hidden_.find(set.Slot()); hidden != hidden_.end()) {
    for (const auto object : hidden->second) {
      if (const auto handle = handle_of(object)) {
        set.Withhold(*handle);
      }
    }
  }

  // The engine's pass already set the bits of entities sent to every client,
  // TransmitAlways ones included, so clearing them here still withholds them.
  for (const auto& [entity, slots] : audiences_) {
    set.WithholdUnlessIn(entity, slots);
  }
  for (const auto& [object, slots] : object_audiences_) {
    if (const auto handle = handle_of(object)) {
      set.WithholdUnlessIn(*handle, slots);
    }
  }
}

}  // namespace modlock::wasm
