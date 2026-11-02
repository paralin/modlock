#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "src/wasm/match/match.pb.h"

namespace modlock::wasm {

// TeamSeating keeps the host's initial seats and the connections that took
// them. Confirmed players are free to change teams under the mod's rules.
class TeamSeating {
 public:
  // Assign replaces the roster atomically; unchanged seats keep confirmation.
  std::expected<void, std::string> Assign(const match::TeamAssignments& request) {
    // Validate the replacement before changing a confirmed seat.
    std::map<uint64_t, int32_t> next;
    for (const auto& [steam_id, team] : request.assignments()) {
      if (!steam_id || team < 1 || team > 3) {
        return std::unexpected("a team assignment needs a Steam ID and team 1, 2 or 3");
      }
      next.emplace(steam_id, team);
    }

    // A changed or removed seat must not reuse a previous confirmation.
    std::erase_if(seated_, [&](const auto& seat) {
      const auto before = assigned_.find(seat.first);
      const auto after = next.find(seat.first);
      return before == assigned_.end() || after == next.end() || before->second != after->second;
    });
    assigned_ = std::move(next);
    return {};
  }

  // Pending returns the team this connection has yet to take, if assigned.
  std::optional<int32_t> Pending(uint64_t steam_id, int32_t slot, uint32_t generation) const {
    // A reconnect does not inherit the previous connection's confirmation.
    const auto seat = assigned_.find(steam_id);
    if (seat == assigned_.end()) {
      return std::nullopt;
    }
    const auto done = seated_.find(steam_id);
    if (done != seated_.end() && done->second == std::pair{slot, generation}) {
      return std::nullopt;
    }
    return seat->second;
  }

  // Confirm records a successful operation only for the current target team.
  void Confirm(uint64_t steam_id, int32_t slot, uint32_t generation, int32_t team) {
    if (Pending(steam_id, slot, generation) == team) {
      seated_[steam_id] = {slot, generation};
    }
  }

  // WorldEnding forgets confirmations when the engine replaces controllers,
  // while retaining the roster for the next map.
  void WorldEnding() { seated_.clear(); }

  // Contains reports whether the host assigned this player a seat.
  bool Contains(uint64_t steam_id) const { return assigned_.contains(steam_id); }

  // Empty reports whether the host leaves all seating to the mod and game.
  bool Empty() const { return assigned_.empty(); }

 private:
  std::map<uint64_t, int32_t> assigned_;
  std::map<uint64_t, std::pair<int32_t, uint32_t>> seated_;
};

}  // namespace modlock::wasm
