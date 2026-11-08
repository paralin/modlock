#pragma once

// HeroPicks remembers the hero each player picked through the game's own
// Change Hero until it lands. The game removes the old hero before it spawns
// the pick, so for a moment the player reads as having no hero. A mod that
// seats a player without a hero would replace their pick in that gap;
// SelectHero asks HeroPicks first and leaves an arriving pick alone.

#include <cstdint>
#include <map>
#include <optional>

namespace modlock::wasm {

class HeroPicks {
 public:
  // kSeconds is how long a pick is waited for after the player asks for it.
  // A pick the game refuses, such as a hero the match does not allow, frees
  // the player for the mod's seat after it.
  static constexpr double kSeconds = 5;

  // Picked notes that the player in slot, on connection generation, asked
  // the game for hero at now, in seconds on a steady clock.
  void Picked(int32_t slot, uint32_t generation, uint32_t hero, double now) {
    picks_.insert_or_assign(slot, Pick{generation, hero, now});
  }

  // Arriving returns the hero the player in slot picked when it has not
  // landed yet: the player is on generation and stands on current, 0 for no
  // hero. A pick that landed, ran out of time or belongs to an earlier
  // connection is forgotten.
  std::optional<uint32_t> Arriving(int32_t slot, uint32_t generation, uint32_t current,
                                   double now) {
    const auto found = picks_.find(slot);
    if (found == picks_.end()) {
      return std::nullopt;
    }
    const Pick pick = found->second;
    if (pick.generation != generation || pick.hero == current || now - pick.at >= kSeconds) {
      picks_.erase(found);
      return std::nullopt;
    }
    return pick.hero;
  }

  // Forget drops the slot's pick, as when the player leaves.
  void Forget(int32_t slot) { picks_.erase(slot); }

 private:
  struct Pick {
    uint32_t generation;
    uint32_t hero;
    double at;
  };

  std::map<int32_t, Pick> picks_;
};

}  // namespace modlock::wasm
