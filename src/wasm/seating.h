#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <set>
#include <string>
#include <string_view>

#include "src/wasm/match.pb.h"
#include "wasm/host_service.gen.h"
#include "wasm/modifier_holds.h"
#include "wasm/module.h"
#include "wasm/roster.h"
#include "wasm/team_seating.h"

namespace modlock::wasm {

class GameServices;

// Seating puts players on teams: the host's seats from its team assignments,
// and, while the mod turns them on, team seating, which seats each player
// without a team on the smaller side, and free-for-all, which also holds
// friendly fire on every hero and makes teammates enemies.
class Seating final : public Module, public SeatingCalls {
 public:
  // Serves reports whether the game controls a player's slot.
  using Serves = std::function<bool(int32_t)>;

  // services, players and modifiers outlive the seating, and serves stays
  // callable while it lives.
  Seating(GameServices& services, Roster& players, ModifierHolds& modifiers, Serves serves);

  Seating(const Seating&) = delete;
  Seating& operator=(const Seating&) = delete;

  std::expected<void, std::string> SetFreeForAll(
      const wasm::SetFreeForAllRequest& request) override;
  std::expected<void, std::string> SetTeamSeating(
      const wasm::SetTeamSeatingRequest& request) override;

  // AssignTeams applies host seats on join and assignment change. The table
  // survives Clear, so a build reload keeps confirmed seats and pending joins.
  std::expected<void, std::string> AssignTeams(const wasm::TeamAssignments& request);

  // StartFreeForAll sets whether free-for-all is on as a mod starts, from its
  // manifest, and turns team seating off. SetFreeForAll and SetTeamSeating
  // change them while the mod runs.
  void StartFreeForAll(bool enabled);

  // FreeForAllCommand reports whether free-for-all consumes a player's console
  // line. Seating is the host's, so it drops the team commands.
  bool FreeForAllCommand(std::string_view line) const;

  // Frame seats the players waiting for a team and holds friendly fire in
  // free-for-all.
  void Frame() override;

  // WorldEnding forgets the world's heroes and reapplies host seats on the
  // next map.
  void WorldEnding() override;

  // Leave forgets that the slot's player could not be seated.
  void Leave(int32_t slot) override;

  // Clear turns free-for-all and team seating off and releases what
  // free-for-all held; the host's assignments stay.
  void Clear() override;

 private:
  // SeatAssignedPlayers retries pending host seats and records a seat only
  // after its native operation succeeds for the same connection generation.
  void SeatAssignedPlayers();
  // SeatAssignedPlayer applies one pending host seat without choosing a hero.
  std::expected<void, std::string> SeatAssignedPlayer(int32_t slot, uint32_t generation,
                                                      uint64_t steam_id, int32_t team);

  // SeatPlayers seats each player who has no team on the smaller side, with
  // no hero, while team seating or free-for-all is on. A host that cannot
  // seat a player leaves the game's own team screen to them.
  void SeatPlayers();
  // ApplyFreeForAll holds friendly fire on every living hero in free-for-all.
  void ApplyFreeForAll();
  // EnableRelations makes teammates enemies to abilities and projectiles,
  // installing the relations on first use.
  void EnableRelations();
  // ReleaseFreeForAll gives back the friendly fire holds free-for-all took
  // and restores the stock team relations.
  void ReleaseFreeForAll();

  GameServices& services_;
  Roster& players_;
  ModifierHolds& modifiers_;
  Serves serves_;
  // free_for_all_ is true while the host seats players and holds friendly fire,
  // and team_seating_ while it only seats them; ffa_held_ is the hero pawns it
  // holds friendly fire on, and seat_failed_ the players it could not seat, so
  // each failure is logged once.
  bool free_for_all_ = false;
  bool team_seating_ = false;
  std::set<uint32_t> ffa_held_;
  std::set<int32_t> seat_failed_;
  // assignments_ survives Clear: the host owns it, independently of the mod.
  TeamSeating assignments_;
};

}  // namespace modlock::wasm
