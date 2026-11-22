#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/vtable_slot_hook.h"

namespace modlock::gameinterop {

// kMaxPlayerSlots bounds the player slots a Deadlock server holds.
inline constexpr int32_t kMaxPlayerSlots = 64;

// IsPlayerSlot reports whether slot names one of the server's player slots.
constexpr bool IsPlayerSlot(int32_t slot) {
  return slot >= 0 && slot < kMaxPlayerSlots;
}

// ClientType is the engine's ClientPutInServer client type. The game passes
// type != 0 as the controller's fakeclient key and type == 2 as its is_hltv key.
enum class ClientType : int {
  kPlayer = 0,
  kBot = 1,
  kSourceTV = 2,
};

// kAwaySeconds is how long a seat stays away before Modlock releases it: the
// game's own citadel_max_disconnected_time, after which a disconnected player
// has abandoned the match.
inline constexpr int kAwaySeconds = 300;

// ConnectionEventSink receives client lifecycle events on the engine thread.
// OnDisconnecting runs while the slot and its world entities are still valid.
//
// A connection is one client's stay on the server. A seat is a player's place
// in the game: their slot, controller and hero, from their first connection
// until they leave for good. The engine holds a human's row after a
// disconnect and gives it, its controller and its hero back only to that
// player, so a disconnect leaves the seat away rather than ending it. A seat
// ends when Modlock releases it (EngineServer::DisconnectClient, or the away
// window running out), when a bot disconnects, or when another player takes
// its slot.
class MODLOCK_API ConnectionEventSink {
 public:
  virtual ~ConnectionEventSink() = default;

  // OnConnected runs after the engine has accepted the client and the tracker
  // has recorded its slot state.
  //
  // is_bot is true for every client type but a player's; SourceTV never
  // connects here. name is the engine-reported client name and may be null or
  // empty.
  virtual void OnConnected(int32_t slot, uint64_t xuid, bool is_bot, const char* name) = 0;

  // OnDisconnecting runs once before the engine begins disconnect teardown.
  // reason is the engine's ENetworkDisconnectionReason (network_connection.proto),
  // such as kDisconnectKicked.
  virtual void OnDisconnecting(int32_t slot, uint64_t xuid, int32_t reason) = 0;

  // OnSeatEnded runs once when slot's seat ends. For a connected client it
  // runs after OnDisconnecting and before the engine's teardown; for an away
  // seat no client is connected, though the seat's controller and hero still
  // read. reason is the disconnect's, the release's, or zero when another
  // player took the away seat's slot.
  virtual void OnSeatEnded(int32_t /*slot*/, uint64_t /*xuid*/, int32_t /*reason*/) {}
};

// ConnectionTracker records the engine's own client lifecycle for player
// slots. It patches ISource2GameClients::ClientPutInServer (slot 13) and
// ClientFullyConnect (slot 15) and ClientDisconnect (slot 16) on server.dll's
// "Source2GameClients001" interface. The SDK declares these callback signatures.
//
// The installed tracker retains the event sink until it clears callback
// registration before restoring the hooks.
class MODLOCK_API ConnectionTracker {
 public:
  // SlotState is the engine-reported connection state for one player slot.
  // The zero value is the empty slot: occupied=false, away=false, xuid=0.
  // occupied means a client is connected. away means none is, but the slot
  // holds a disconnected human's seat; it keeps that seat's xuid, generation
  // and name, and is never occupied or fully connected.
  struct SlotState {
    bool occupied = false;
    bool away = false;
    uint64_t xuid = 0;
    // generation names the slot's seat: it changes when a new seat starts,
    // and a player returning to their away seat keeps it.
    uint32_t generation = 0;
    bool is_bot = false;
    std::string name;
    // Humans become ready at ClientFullyConnect; native bots have no sign-on exchange.
    bool fully_connected = false;
  };

  // EngineCallback is the displaced engine method in synthetic hook tests.
  using EngineCallback = std::function<void()>;

  // Install resolves server.dll's game-clients interface and patches its
  // PutInServer, FullyConnect and Disconnect entries. sink is retained until all hooks are
  // restored; a null sink is rejected.
  static std::expected<ConnectionTracker, std::string> Install(
      std::shared_ptr<ConnectionEventSink> sink);

  // DispatchPutInServer applies the live thunk order to a synthetic engine
  // callback: original, state recording, then OnConnected. type and name
  // carry the engine callback's own client type and name arguments. SourceTV
  // takes a player slot but is no player, so its slot stays unoccupied and the
  // sink never hears of it.
  static void DispatchPutInServer(int32_t slot, uint64_t xuid, ClientType type, const char* name,
                                  const EngineCallback& original,
                                  std::shared_ptr<ConnectionEventSink> sink);

  // DispatchDisconnect applies the live thunk order to a synthetic engine
  // callback: capture occupied state, OnDisconnecting with reason,
  // OnSeatEnded when the seat ends with the connection, original, then
  // record the slot away or empty.
  static void DispatchDisconnect(int32_t slot, int32_t reason, const EngineCallback& original,
                                 std::shared_ptr<ConnectionEventSink> sink);

  // Release ends slot's seat for good. EngineServer::DisconnectClient calls
  // it before the engine's disconnect. A connected client's seat ends at that
  // disconnect, whenever the engine runs it; an away seat ends now, with
  // OnSeatEnded. DispatchRelease takes the sink explicitly for tests.
  static void Release(int32_t slot, int32_t reason);
  static void DispatchRelease(int32_t slot, int32_t reason,
                              std::shared_ptr<ConnectionEventSink> sink);

  // ExpiredAway lists the slots whose seats have been away for kAwaySeconds
  // by now, for the host to release.
  static std::vector<int32_t> ExpiredAway(std::chrono::steady_clock::time_point now);

  // DispatchFullyConnect publishes readiness after the engine's sign-on
  // callback, provided it still belongs to the same occupied generation.
  static void DispatchFullyConnect(int32_t slot, const EngineCallback& original);

  // StateForSlot returns the last lifecycle event the engine reported for
  // slot; an empty tracker reports every slot unoccupied with xuid 0.
  static SlotState StateForSlot(int32_t slot);

  // SeatGeneration returns the generation of slot's latest seat, current or
  // ended, so a caller holding an ended seat's generation can tell when a new
  // seat starts there.
  static uint64_t SeatGeneration(int32_t slot);

  // Revision changes after each join, finished join, disconnect and seat
  // end, so a reader
  // knows when StateForSlot answers differently for some slot.
  static uint64_t Revision();

  // ApplyPutInServer records one engine ClientPutInServer callback. It is the
  // recording half of PutInServerThunk; tests pin it against recorded
  // lifecycles without a live interface.
  static void ApplyPutInServer(int32_t slot, uint64_t xuid, bool is_bot, const char* name);

  // ApplyDisconnect records one engine ClientDisconnect callback: a human's
  // seat goes away at now, and a bot's or a released seat ends.
  static void ApplyDisconnect(
      int32_t slot, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  ConnectionTracker(ConnectionTracker&& other) noexcept;
  ConnectionTracker& operator=(ConnectionTracker&& other) noexcept;
  ~ConnectionTracker();

  ConnectionTracker(const ConnectionTracker&) = delete;
  ConnectionTracker& operator=(const ConnectionTracker&) = delete;

 private:
  // Private by construction: only Install builds one, around live slot hooks.
  ConnectionTracker(VtableSlotHook put_in_server, VtableSlotHook fully_connect,
                    VtableSlotHook disconnect, std::shared_ptr<ConnectionEventSink> sink);

  void Reset();

  VtableSlotHook put_in_server_;
  VtableSlotHook fully_connect_;
  VtableSlotHook disconnect_;
  std::shared_ptr<ConnectionEventSink> sink_;
  bool active_ = true;
};

}  // namespace modlock::gameinterop
