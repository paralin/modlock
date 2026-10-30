#pragma once

#include <expected>
#include <string>

#include "modlock/export.h"

namespace modlock::gameinterop {

// FullUpdates makes every connected client take a full update, in which the
// client deletes and creates every entity again. A client builds some state
// only when it creates an entity, such as a client UI entity's panel, so a full
// update is how a change to that state reaches players already connected.
//
// The engine's own sv_fullupdate does not do this in the game's release
// build: it asks each client for tick -1, which the server now keeps as the
// client's current delta tick. RequestAll instead clears the delta tick
// itself, so the next snapshot to each client is a full one.
class MODLOCK_API FullUpdates {
 public:
  // Resolve finds the network game server's global in the mapped engine2.dll.
  static std::expected<FullUpdates, std::string> Resolve();

  // RequestAll asks the server to send each connected client a full update
  // with its next snapshot. It runs on the engine thread, between frames.
  void RequestAll() const;

 private:
  // server_ is engine2's global that holds the network game server, or null
  // while no server runs.
  void* const* server_ = nullptr;
};

}  // namespace modlock::gameinterop
