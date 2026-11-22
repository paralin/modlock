#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "google/protobuf/repeated_ptr_field.h"
#include "src/wasm/players/players.pb.h"

namespace modlock::wasm {

// Attendance is the players one mod instance heard join and not yet leave, so
// the mod hears exactly one PlayerLeft for each PlayerJoined. A seat joins
// once it is ready and the mod serves it, and leaves when it ends, when the
// mod stops serving it, or, should the host miss both, when a later roster no
// longer lists it or lists a new seat in its slot. In between, a joined seat
// goes away when the roster lists it away or not ready, and returns when the
// roster lists it ready again, each heard once.
class Attendance {
 public:
  // Changes is what one roster changed: the joined seats it no longer lists,
  // which left unseen, the ready ones that join, and the joined ones that go
  // away or return.
  struct Changes {
    std::vector<players::PlayerLeftEvent> left;
    std::vector<players::PlayerJoinedEvent> joined;
    std::vector<players::PlayerAwayEvent> away;
    std::vector<players::PlayerReturnedEvent> returned;
  };

  // Reconcile records roster, the connections the mod serves now, and
  // returns what changed since the last roster and leave. A joined
  // connection it still lists takes the roster's copy, such as new roles.
  Changes Reconcile(const google::protobuf::RepeatedPtrField<players::Connection>& roster);

  // Leave forgets the joined connection in slot and returns its leave, or
  // nullopt when none joined there. reason is the engine's disconnect reason
  // when the player leaves the server, and nullopt when the player stays but
  // the mod stops serving them.
  std::optional<players::PlayerLeftEvent> Leave(int32_t slot, std::optional<int32_t> reason);

  // Clear forgets every connection, for a new instance that has heard none.
  void Clear() { joined_.clear(); }

 private:
  // Seat is a joined seat: its last listing, and whether the mod heard it
  // go away and not yet return.
  struct Seat {
    players::Connection connection;
    bool away = false;
  };

  // joined_ holds each joined seat by slot.
  std::map<int32_t, Seat> joined_;
};

}  // namespace modlock::wasm
