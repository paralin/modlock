#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

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

// ConnectionEventSink receives client lifecycle events on the engine thread.
// OnDisconnecting runs while the slot and its world entities are still valid.
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
  virtual void OnDisconnecting(int32_t slot, uint64_t xuid) = 0;
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
  // The zero value is the unoccupied slot: occupied=false, xuid=0, empty name.
  struct SlotState {
    bool occupied = false;
    uint64_t xuid = 0;
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
  // callback: capture occupied state, OnDisconnecting, original, then clear.
  static void DispatchDisconnect(int32_t slot, const EngineCallback& original,
                                 std::shared_ptr<ConnectionEventSink> sink);

  // DispatchFullyConnect publishes readiness after the engine's sign-on
  // callback, provided it still belongs to the same occupied generation.
  static void DispatchFullyConnect(int32_t slot, const EngineCallback& original);

  // StateForSlot returns the last lifecycle event the engine reported for
  // slot; an empty tracker reports every slot unoccupied with xuid 0.
  static SlotState StateForSlot(int32_t slot);

  // Revision changes after each join, finished join and leave, so a reader
  // knows when StateForSlot answers differently for some slot.
  static uint64_t Revision();

  // ApplyPutInServer records one engine ClientPutInServer callback. It is the
  // recording half of PutInServerThunk; tests pin it against recorded
  // lifecycles without a live interface.
  static void ApplyPutInServer(int32_t slot, uint64_t xuid, bool is_bot, const char* name);

  // ApplyDisconnect records one engine ClientDisconnect callback.
  static void ApplyDisconnect(int32_t slot);

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
