#include "modlock/gameinterop/connection_tracker.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace modlock::gameinterop {
namespace {

// ABSOLUTE_PLAYER_LIMIT (sourcesdk public/const.h).
inline constexpr int32_t kMaxSlots = 64;

// Server.dll's game-clients interface version (sourcesdk public/eiface.h).
inline constexpr char kSource2GameClientsVersion[] = "Source2GameClients001";

// g_occupied[slot] is true while a client is connected there, and g_away[slot]
// while no client is but a disconnected human's seat holds the slot, since
// g_away_since[slot] (steady_clock ticks). g_releasing[slot] marks a connected
// seat Release ended, which its disconnect then ends rather than sends away.
std::array<std::atomic<bool>, kMaxSlots> g_occupied{};
std::array<std::atomic<bool>, kMaxSlots> g_away{};
std::array<std::atomic<int64_t>, kMaxSlots> g_away_since{};
std::array<std::atomic<bool>, kMaxSlots> g_releasing{};
std::array<std::atomic<bool>, kMaxSlots> g_disconnect_claimed{};
std::array<std::atomic<uint64_t>, kMaxSlots> g_xuid{};
// g_generation[slot] counts the seats that started in slot; g_connection[slot]
// counts its connections, so a dispatch can tell a client that arrived during
// its callback, a return included, from the one it began with.
std::array<std::atomic<uint64_t>, kMaxSlots> g_generation{};
std::array<std::atomic<uint64_t>, kMaxSlots> g_connection{};
std::array<std::atomic<bool>, kMaxSlots> g_is_bot{};
std::array<std::atomic<bool>, kMaxSlots> g_fully_connected{};
// g_name[slot] is written only on the engine thread between a PutInServer and
// its slot's next lifecycle event; readers on other threads may observe a
// torn or stale name, which every consumer treats as diagnostic-only.
std::array<std::string, kMaxSlots> g_name{};
// g_address[slot] is the address the slot's latest client connected from, as
// OnClientConnected reports it, such as 127.0.0.1:27006. It is written on the
// engine thread before that client's PutInServer, with g_name's tearing rule.
std::array<std::string, kMaxSlots> g_address{};
// g_revision counts the lifecycle events that changed a slot's state.
std::atomic<uint64_t> g_revision{0};

std::atomic<void*> g_connected_original{nullptr};
std::atomic<void*> g_put_in_server_original{nullptr};
std::atomic<void*> g_fully_connect_original{nullptr};
std::atomic<void*> g_disconnect_original{nullptr};
std::shared_ptr<ConnectionEventSink> g_event_sink;

bool ValidSlot(int32_t slot) {
  return slot >= 0 && slot < kMaxSlots;
}

// Thunk signatures follow sourcesdk public/eiface.h: CPlayerSlot is a single-
// int POD, so the x64 ABI passes it as an int register argument.
using ConnectedFn = void (*)(void* self, int32_t slot, const char* name, uint64_t xuid,
                            const char* network_id, const char* address, bool fake);
using PutInServerFn = void (*)(void* self, int32_t slot, const char* name, int type, uint64_t xuid);
using FullyConnectFn = void (*)(void* self, int32_t slot);
using DisconnectFn = void (*)(void* self, int32_t slot, int reason, const char* name, uint64_t xuid,
                              const char* network_id);

void ConnectedThunk(void* self, int32_t slot, const char* name, uint64_t xuid,
                    const char* network_id, const char* address, bool fake) {
  ConnectionTracker::ApplyConnected(slot, address);
  void* original = g_connected_original.load(std::memory_order_acquire);
  if (original != nullptr) {
    reinterpret_cast<ConnectedFn>(original)(self, slot, name, xuid, network_id, address, fake);
  }
}

void PutInServerThunk(void* self, int32_t slot, const char* name, int type, uint64_t xuid) {
  void* original = g_put_in_server_original.load(std::memory_order_acquire);
  ConnectionTracker::DispatchPutInServer(
      slot, xuid, static_cast<ClientType>(type), name,
      [&, original] {
        if (original != nullptr) {
          reinterpret_cast<PutInServerFn>(original)(self, slot, name, type, xuid);
        }
      },
      std::atomic_load_explicit(&g_event_sink, std::memory_order_acquire));
}

void FullyConnectThunk(void* self, int32_t slot) {
  void* original = g_fully_connect_original.load(std::memory_order_acquire);
  ConnectionTracker::DispatchFullyConnect(slot, [=] {
    if (original) {
      reinterpret_cast<FullyConnectFn>(original)(self, slot);
    }
  });
}

void DisconnectThunk(void* self, int32_t slot, int reason, const char* name, uint64_t xuid,
                     const char* network_id) {
  void* original = g_disconnect_original.load(std::memory_order_acquire);
  ConnectionTracker::DispatchDisconnect(
      slot, reason,
      [&, original] {
        if (original != nullptr) {
          reinterpret_cast<DisconnectFn>(original)(self, slot, reason, name, xuid, network_id);
        }
      },
      std::atomic_load_explicit(&g_event_sink, std::memory_order_acquire));
}

}  // namespace

ConnectionTracker::ConnectionTracker(std::optional<VtableSlotHook> connected,
                                     VtableSlotHook put_in_server, VtableSlotHook fully_connect,
                                     VtableSlotHook disconnect,
                                     std::shared_ptr<ConnectionEventSink> sink)
    : connected_(std::move(connected)),
      put_in_server_(std::move(put_in_server)),
      fully_connect_(std::move(fully_connect)),
      disconnect_(std::move(disconnect)),
      sink_(std::move(sink)) {}

ConnectionTracker::ConnectionTracker(ConnectionTracker&& other) noexcept
    : connected_(std::move(other.connected_)),
      put_in_server_(std::move(other.put_in_server_)),
      fully_connect_(std::move(other.fully_connect_)),
      disconnect_(std::move(other.disconnect_)),
      sink_(std::move(other.sink_)),
      active_(other.active_) {
  other.active_ = false;
}

ConnectionTracker& ConnectionTracker::operator=(ConnectionTracker&& other) noexcept {
  if (this != &other) {
    // Release our hooks, then take the other's.
    Reset();
    connected_ = std::move(other.connected_);
    put_in_server_ = std::move(other.put_in_server_);
    fully_connect_ = std::move(other.fully_connect_);
    disconnect_ = std::move(other.disconnect_);
    sink_ = std::move(other.sink_);
    active_ = other.active_;
    other.active_ = false;
  }
  return *this;
}

ConnectionTracker::~ConnectionTracker() {
  Reset();
}

void ConnectionTracker::Reset() {
  // Only an installed tracker restores.
  if (!active_) {
    return;
  }

  // Drop the sink before restoring the dispatch entries. A concurrent thunk
  // can therefore never call an object after its installed tracker releases it.
  std::atomic_store_explicit(&g_event_sink, std::shared_ptr<ConnectionEventSink>{},
                             std::memory_order_release);

  // VtableSlotHook::Restore has no success result. Keep displaced originals
  // callable if a thunk remains installed after a failed protection/write.
  if (connected_) {
    connected_->Restore();
  }
  put_in_server_.Restore();
  fully_connect_.Restore();
  disconnect_.Restore();
  sink_.reset();
  active_ = false;
}

void ConnectionTracker::DispatchPutInServer(int32_t slot, uint64_t xuid, ClientType type,
                                            const char* name, const EngineCallback& original,
                                            std::shared_ptr<ConnectionEventSink> sink) {
  // These are connection receipts, not frame diagnostics. The server operator
  // needs every join, including a full roster and later reconnects.
  if (InteropTraceEnabled()) {
    std::fprintf(stderr, "[modlock] interop trace: ClientPutInServer slot %d xuid %llu type %d\n",
                 slot, static_cast<unsigned long long>(xuid), static_cast<int>(type));
    std::fflush(stderr);
  }
  if (original) {
    original();
  }

  // SourceTV is no player: leave its slot unoccupied, so no mod seats, kicks
  // or lists it. Its disconnect then takes the path for a slot never joined.
  if (type == ClientType::kSourceTV) {
    return;
  }
  // An away seat whose slot another player takes has ended: only its owner
  // returns to it.
  const bool is_bot = type != ClientType::kPlayer;
  if (ValidSlot(slot) && g_away[slot].load(std::memory_order_acquire) &&
      (is_bot || g_xuid[slot].load(std::memory_order_relaxed) != xuid)) {
    const uint64_t owner = g_xuid[slot].load(std::memory_order_relaxed);
    g_away[slot].store(false, std::memory_order_release);
    g_revision.fetch_add(1, std::memory_order_release);
    if (sink != nullptr) {
      sink->OnSeatEnded(slot, owner, 0);
    }
  }
  ApplyPutInServer(slot, xuid, is_bot, name);
  if (ValidSlot(slot) && sink != nullptr) {
    sink->OnConnected(slot, xuid, is_bot, name);
  }
}

void ConnectionTracker::DispatchFullyConnect(int32_t slot, const EngineCallback& original) {
  // Run the engine's call, noting the connection before and after.
  if (!ValidSlot(slot)) {
    if (original) {
      original();
    }
    return;
  }
  const bool occupied = g_occupied[slot].load(std::memory_order_acquire);
  const uint64_t connection = g_connection[slot].load(std::memory_order_acquire);
  if (original) {
    original();
  }

  // Mark it fully connected only if the same client stayed.
  if (!occupied || !g_occupied[slot].load(std::memory_order_acquire) ||
      g_connection[slot].load(std::memory_order_acquire) != connection) {
    return;
  }
  g_fully_connected[slot].store(true, std::memory_order_release);
  g_revision.fetch_add(1, std::memory_order_release);
  if (InteropTraceEnabled()) {
    std::fprintf(stderr, "[modlock] interop trace: ClientFullyConnect slot %d\n", slot);
    std::fflush(stderr);
  }
}

void ConnectionTracker::DispatchDisconnect(int32_t slot, int32_t reason,
                                           const EngineCallback& original,
                                           std::shared_ptr<ConnectionEventSink> sink) {
  // Every disconnect is logged with the engine's reason, an
  // ENetworkDisconnectionReason, since no other record names why a client was
  // dropped. A client still loading never reaches a mod's leave event, so this
  // line is the only record of its reason. One line per disconnect.
  const auto leaving = StateForSlot(slot);
  std::fprintf(stderr, "[modlock] ClientDisconnect slot %d reason %d%s\n", slot, reason,
               leaving.occupied && !leaving.fully_connected ? " before fully connecting" : "");
  std::fflush(stderr);

  // A slot with no client runs the engine call and keeps its state: an away
  // seat stays away, and an empty slot stays empty.
  const SlotState state = StateForSlot(slot);
  if (!state.occupied || !ValidSlot(slot)) {
    if (original) {
      original();
    }
    return;
  }

  // Only the outer dispatch of a nested disconnect proceeds.
  bool expected = false;
  if (!g_disconnect_claimed[slot].compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
    // The outer dispatch retains the slot claim and will complete the engine
    // call and state transition exactly once.
    return;
  }

  // Tell the sink, and when the seat ends with the connection, say so too;
  // the sink may release the seat from its callback. Then run the engine's
  // call.
  const uint64_t connection = g_connection[slot].load(std::memory_order_acquire);
  if (sink != nullptr) {
    sink->OnDisconnecting(slot, state.xuid, reason);
  }
  if (sink != nullptr && (state.is_bot || g_releasing[slot].load(std::memory_order_acquire))) {
    sink->OnSeatEnded(slot, state.xuid, reason);
  }
  if (original) {
    original();
  }

  // A reconnect during the sink callback is a new connection. Do not erase
  // that replacement client's occupied state when the old disconnect returns.
  if (g_connection[slot].load(std::memory_order_acquire) == connection) {
    ApplyDisconnect(slot);
    g_disconnect_claimed[slot].store(false, std::memory_order_release);
  }
}

void ConnectionTracker::Release(int32_t slot, int32_t reason) {
  DispatchRelease(slot, reason,
                  std::atomic_load_explicit(&g_event_sink, std::memory_order_acquire));
}

void ConnectionTracker::DispatchRelease(int32_t slot, int32_t reason,
                                        std::shared_ptr<ConnectionEventSink> sink) {
  if (!ValidSlot(slot)) {
    return;
  }

  // A connected seat ends at its disconnect, which the release precedes.
  if (g_occupied[slot].load(std::memory_order_acquire)) {
    g_releasing[slot].store(true, std::memory_order_release);
    return;
  }

  // An away seat ends now. Clear it before telling the sink, so a release
  // from the callback finds no seat to end twice.
  if (!g_away[slot].exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  g_revision.fetch_add(1, std::memory_order_release);
  std::fprintf(stderr, "[modlock] seat released: slot %d after its player left\n", slot);
  std::fflush(stderr);
  if (sink != nullptr) {
    sink->OnSeatEnded(slot, g_xuid[slot].load(std::memory_order_relaxed), reason);
  }
}

std::vector<int32_t> ConnectionTracker::ExpiredAway(std::chrono::steady_clock::time_point now) {
  std::vector<int32_t> expired;
  const auto window = std::chrono::steady_clock::duration(std::chrono::seconds(kAwaySeconds));
  for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
    if (!g_away[slot].load(std::memory_order_acquire)) {
      continue;
    }
    const std::chrono::steady_clock::time_point since{
        std::chrono::steady_clock::duration(g_away_since[slot].load(std::memory_order_relaxed))};
    if (now - since >= window) {
      expired.push_back(slot);
    }
  }
  return expired;
}

void ConnectionTracker::ApplyConnected(int32_t slot, const char* address) {
  if (ValidSlot(slot)) {
    g_address[slot] = (address != nullptr) ? std::string(address) : std::string();
  }
}

void ConnectionTracker::ApplyPutInServer(int32_t slot, uint64_t xuid, bool is_bot,
                                         const char* name) {
  // Record the client in its slot. The owner of an away seat returns to it,
  // keeping its generation; any other client starts a new seat.
  if (!ValidSlot(slot)) {
    return;
  }
  const bool returning = g_away[slot].load(std::memory_order_acquire) && !is_bot &&
                         g_xuid[slot].load(std::memory_order_relaxed) == xuid;
  g_xuid[slot].store(xuid, std::memory_order_relaxed);
  g_is_bot[slot].store(is_bot, std::memory_order_relaxed);
  g_fully_connected[slot].store(is_bot, std::memory_order_relaxed);
  g_name[slot] = (name != nullptr) ? std::string(name) : std::string();
  if (!returning) {
    g_generation[slot].fetch_add(1, std::memory_order_acq_rel);
  }
  g_connection[slot].fetch_add(1, std::memory_order_acq_rel);

  // A reconnect starts a new disconnect lifecycle, even when it occurs from
  // the previous connection's OnDisconnecting callback. Publish this claim
  // reset before occupied=true so a new disconnect cannot miss the connection.
  g_releasing[slot].store(false, std::memory_order_release);
  g_disconnect_claimed[slot].store(false, std::memory_order_release);
  g_away[slot].store(false, std::memory_order_release);
  g_occupied[slot].store(true, std::memory_order_release);
  g_revision.fetch_add(1, std::memory_order_release);
}

void ConnectionTracker::ApplyDisconnect(int32_t slot, std::chrono::steady_clock::time_point now) {
  if (!ValidSlot(slot) || !g_occupied[slot].load(std::memory_order_acquire)) {
    return;
  }

  // The engine holds a human's row for their return, so their seat goes
  // away; a bot's row is freed, and a released seat ends.
  const bool ends = g_is_bot[slot].load(std::memory_order_relaxed) ||
                    g_releasing[slot].exchange(false, std::memory_order_acq_rel);
  if (!ends) {
    g_away_since[slot].store(now.time_since_epoch().count(), std::memory_order_relaxed);
  }
  g_away[slot].store(!ends, std::memory_order_release);
  g_occupied[slot].store(false, std::memory_order_release);
  g_revision.fetch_add(1, std::memory_order_release);
}

uint64_t ConnectionTracker::SeatGeneration(int32_t slot) {
  return ValidSlot(slot) ? g_generation[slot].load(std::memory_order_acquire) : 0;
}

uint64_t ConnectionTracker::Revision() {
  return g_revision.load(std::memory_order_acquire);
}

ConnectionTracker::SlotState ConnectionTracker::StateForSlot(int32_t slot) {
  if (!ValidSlot(slot)) {
    return SlotState{};
  }
  if (!g_occupied[slot].load(std::memory_order_acquire)) {
    if (!g_away[slot].load(std::memory_order_acquire)) {
      return SlotState{};
    }
    return SlotState{
        .away = true,
        .xuid = g_xuid[slot].load(std::memory_order_relaxed),
        .generation = static_cast<uint32_t>(g_generation[slot].load(std::memory_order_acquire)),
        .name = g_name[slot]};
  }
  return SlotState{
      .occupied = true,
      .xuid = g_xuid[slot].load(std::memory_order_relaxed),
      .generation = static_cast<uint32_t>(g_generation[slot].load(std::memory_order_acquire)),
      .is_bot = g_is_bot[slot].load(std::memory_order_relaxed),
      .name = g_name[slot],
      .address = g_address[slot],
      .fully_connected = g_fully_connected[slot].load(std::memory_order_acquire)};
}

std::expected<ConnectionTracker, std::string> ConnectionTracker::Install(
    std::shared_ptr<ConnectionEventSink> sink) {
  // Refuse a missing sink.
  if (sink == nullptr) {
    return std::unexpected("connection tracker requires an event sink");
  }

  // Find the game clients interface.
  const auto resolved = ResolveEngineInterface(L"server.dll", kSource2GameClientsVersion);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  void* clients = *resolved;

  // Find its three slots.
  const auto put_in_server_slot = ResolveVirtualSlot("game-clients.put-in-server");
  const auto fully_connect_slot = ResolveVirtualSlot("game-clients.fully-connect");
  const auto disconnect_slot = ResolveVirtualSlot("game-clients.disconnect");
  if (!put_in_server_slot) {
    return std::unexpected(put_in_server_slot.error());
  }
  if (!fully_connect_slot) {
    return std::unexpected(fully_connect_slot.error());
  }
  if (!disconnect_slot) {
    return std::unexpected(disconnect_slot.error());
  }

  // Hook each slot. The connect hook only reports addresses, so a build where
  // it does not resolve tracks connections without them.
  std::optional<VtableSlotHook> connected;
  if (const auto slot = ResolveVirtualSlot("game-clients.on-connected"); slot) {
    if (auto hook = VtableSlotHook::Install(clients, *slot, reinterpret_cast<void*>(&ConnectedThunk));
        hook) {
      g_connected_original.store(hook->Original(), std::memory_order_release);
      connected = std::move(*hook);
    } else {
      std::fprintf(stderr, "[modlock] client addresses unavailable: %s\n", hook.error().c_str());
    }
  } else {
    std::fprintf(stderr, "[modlock] client addresses unavailable: %s\n", slot.error().c_str());
  }
  auto put_in_server = VtableSlotHook::Install(clients, *put_in_server_slot,
                                               reinterpret_cast<void*>(&PutInServerThunk));
  if (!put_in_server) {
    return std::unexpected(put_in_server.error());
  }
  auto fully_connect = VtableSlotHook::Install(clients, *fully_connect_slot,
                                               reinterpret_cast<void*>(&FullyConnectThunk));
  if (!fully_connect) {
    return std::unexpected(fully_connect.error());
  }
  auto disconnect =
      VtableSlotHook::Install(clients, *disconnect_slot, reinterpret_cast<void*>(&DisconnectThunk));
  if (!disconnect) {
    return std::unexpected(disconnect.error());
  }

  // Publish the originals and the sink to the thunks.
  g_put_in_server_original.store(put_in_server->Original(), std::memory_order_release);
  g_fully_connect_original.store(fully_connect->Original(), std::memory_order_release);
  g_disconnect_original.store(disconnect->Original(), std::memory_order_release);
  std::atomic_store_explicit(&g_event_sink, sink, std::memory_order_release);
  return ConnectionTracker(std::move(connected), std::move(*put_in_server), std::move(*fully_connect),
                           std::move(*disconnect), std::move(sink));
}

}  // namespace modlock::gameinterop
