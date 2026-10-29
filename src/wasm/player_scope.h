#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "modlock/wasm_host.h"
#include "src/wasm/players/players.pb.h"
#include "src/wasm/wasm.pb.h"

namespace modlock::wasm {

// PlayerScope limits a mod to a set of players, as when one mod runs a game
// for some of a server's players beside another mod. A limited mod sees only
// its players in the player list, hears no event about another player, and
// is refused a request that addresses another player or, unless the scope
// holds the world, changes the whole server. An unlimited scope serves every
// player and holds the world.
//
// A message field named player or players holds client slots; -1 names no
// player. The scope reads those fields from each request and event by its
// method's message type, so new methods are covered without a list. Entities
// and world objects are not players: a limited mod still reaches them.
class PlayerScope {
 public:
  // Limit serves scope's players, or every player when they are nullopt.
  void Limit(WasmScope scope) { scope_ = std::move(scope); }

  // Limited reports whether the scope serves only some players.
  [[nodiscard]] bool Limited() const { return scope_.players.has_value(); }

  // World reports whether the mod may change the game for every player.
  [[nodiscard]] bool World() const { return !Limited() || scope_.world; }

  // Apart returns the slot bits of the players hidden from everyone else, or
  // zero when the scope is not apart.
  [[nodiscard]] uint64_t Apart() const;

  // Contact is what a mod does with a hit.
  enum class Contact {
    // kHear: the hit is the mod's to hear and rule.
    kHear,
    // kPass: the hit belongs to no player the mod serves; it neither hears
    // nor rules it.
    kPass,
    // kBlock: the hit crosses from one of the mod's players to a player it
    // does not serve, or back, so no mod may let it land.
    kBlock,
  };

  // Hit returns what the mod does with a hit by the player in attacker on the
  // player in victim, each nullopt for an entity that is no player's hero,
  // such as a trooper, a structure or a hazard. A hit with one player belongs
  // to that player's game, and one with none to unlimited mods alone.
  [[nodiscard]] Contact Hit(std::optional<int32_t> attacker, std::optional<int32_t> victim) const;

  // Serves reports whether the scope serves the player in slot.
  [[nodiscard]] bool Serves(int32_t slot) const {
    return !scope_.players || slot < 0 || scope_.players->contains(slot);
  }

  // Refuse returns why the mod may not make the host request call, or an
  // empty string when it may.
  [[nodiscard]] std::string Refuse(const Call& call) const;

  // Hears reports whether the mod hears the event call.
  [[nodiscard]] bool Hears(const Call& event) const;

  // Keep drops the players the scope does not serve from a player list.
  void Keep(google::protobuf::RepeatedPtrField<players::Connection>& players) const;

 private:
  // Addresses reports whether message, of the named method's request type in
  // service, names only players the scope serves.
  [[nodiscard]] bool Addresses(std::string_view service, std::string_view method,
                               const std::string& message) const;

  WasmScope scope_;
};

}  // namespace modlock::wasm
