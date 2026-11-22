//go:build deps_only

#include "wasm/attendance.h"

#include <set>
#include <utility>

namespace modlock::wasm {

namespace {

// Present reports whether connection is a seat whose player is here: ready,
// and not away.
bool Present(const players::Connection& connection) {
  return connection.ready() && !connection.away();
}

// LeftEvent is the leave of connection. reason is set only for a disconnect
// the engine explained, since zero is the engine's invalid reason; a leave
// the roster reveals is a disconnect without one.
players::PlayerLeftEvent LeftEvent(players::Connection connection, bool disconnected,
                                   std::optional<int32_t> reason) {
  players::PlayerLeftEvent event;
  *event.mutable_connection() = std::move(connection);
  event.set_disconnected(disconnected);
  if (reason && *reason != players::DISCONNECT_REASON_UNKNOWN) {
    event.set_reason(static_cast<players::DisconnectReason>(*reason));
  }
  return event;
}

}  // namespace

Attendance::Changes Attendance::Reconcile(
    const google::protobuf::RepeatedPtrField<players::Connection>& roster) {
  Changes changes;
  std::set<int32_t> listed;
  for (const players::Connection& connection : roster) {
    listed.insert(connection.player());
  }
  for (auto it = joined_.begin(); it != joined_.end();) {
    if (listed.count(it->first) == 0) {
      changes.left.push_back(LeftEvent(std::move(it->second.connection), true, std::nullopt));
      it = joined_.erase(it);
    } else {
      ++it;
    }
  }
  for (const players::Connection& connection : roster) {
    // A new seat in a joined slot ends the old one.
    auto it = joined_.find(connection.player());
    if (it != joined_.end() && it->second.connection.generation() != connection.generation()) {
      changes.left.push_back(LeftEvent(std::move(it->second.connection), true, std::nullopt));
      joined_.erase(it);
      it = joined_.end();
    }

    // A ready seat the mod has not heard joins.
    const bool present = Present(connection);
    if (it == joined_.end()) {
      if (present) {
        joined_.emplace(connection.player(), Seat{.connection = connection});
        players::PlayerJoinedEvent joined;
        *joined.mutable_connection() = connection;
        changes.joined.push_back(std::move(joined));
      }
      continue;
    }

    // A joined seat takes the roster's copy, and goes away or returns.
    Seat& seat = it->second;
    seat.connection = connection;
    if (seat.away == !present) {
      continue;
    }
    seat.away = !present;
    if (seat.away) {
      *changes.away.emplace_back().mutable_connection() = connection;
    } else {
      *changes.returned.emplace_back().mutable_connection() = connection;
    }
  }
  return changes;
}

std::optional<players::PlayerLeftEvent> Attendance::Leave(int32_t slot,
                                                          std::optional<int32_t> reason) {
  auto it = joined_.find(slot);
  if (it == joined_.end()) {
    return std::nullopt;
  }
  players::PlayerLeftEvent event =
      LeftEvent(std::move(it->second.connection), reason.has_value(), reason);
  joined_.erase(it);
  return event;
}

}  // namespace modlock::wasm
