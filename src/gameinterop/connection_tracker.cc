#include "modlock/gameinterop/connection_tracker.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

std::array<std::atomic<bool>, kMaxSlots> g_occupied{};
std::array<std::atomic<bool>, kMaxSlots> g_disconnect_claimed{};
std::array<std::atomic<uint64_t>, kMaxSlots> g_xuid{};
std::array<std::atomic<uint64_t>, kMaxSlots> g_generation{};
std::array<std::atomic<bool>, kMaxSlots> g_is_bot{};
std::array<std::atomic<bool>, kMaxSlots> g_fully_connected{};
// g_name[slot] is written only on the engine thread between a PutInServer and
// its slot's next lifecycle event; readers on other threads may observe a
// torn or stale name, which every consumer treats as diagnostic-only.
std::array<std::string, kMaxSlots> g_name{};
// g_revision counts the lifecycle events that changed a slot's state.
std::atomic<uint64_t> g_revision{0};

std::atomic<void*> g_put_in_server_original{nullptr};
std::atomic<void*> g_fully_connect_original{nullptr};
std::atomic<void*> g_disconnect_original{nullptr};
std::shared_ptr<ConnectionEventSink> g_event_sink;

bool ValidSlot(int32_t slot) {
  return slot >= 0 && slot < kMaxSlots;
}

// Thunk signatures follow sourcesdk public/eiface.h: CPlayerSlot is a single-
// int POD, so the x64 ABI passes it as an int register argument.
using PutInServerFn = void (*)(void* self, int32_t slot, const char* name, int type, uint64_t xuid);
using FullyConnectFn = void (*)(void* self, int32_t slot);
using DisconnectFn = void (*)(void* self, int32_t slot, int reason, const char* name, uint64_t xuid,
                              const char* network_id);

void PutInServerThunk(void* self, int32_t slot, const char* name, int type, uint64_t xuid) {
  void* original = g_put_in_server_original.load(std::memory_order_acquire);
  // The engine's nonzero client type identifies a bot.
  ConnectionTracker::DispatchPutInServer(
      slot, xuid, type != 0, name,
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
      slot,
      [&, original] {
        if (original != nullptr) {
          reinterpret_cast<DisconnectFn>(original)(self, slot, reason, name, xuid, network_id);
        }
      },
      std::atomic_load_explicit(&g_event_sink, std::memory_order_acquire));
}

}  // namespace

ConnectionTracker::ConnectionTracker(VtableSlotHook put_in_server, VtableSlotHook fully_connect,
                                     VtableSlotHook disconnect,
                                     std::shared_ptr<ConnectionEventSink> sink)
    : put_in_server_(std::move(put_in_server)),
      fully_connect_(std::move(fully_connect)),
      disconnect_(std::move(disconnect)),
      sink_(std::move(sink)) {}

ConnectionTracker::ConnectionTracker(ConnectionTracker&& other) noexcept
    : put_in_server_(std::move(other.put_in_server_)),
      fully_connect_(std::move(other.fully_connect_)),
      disconnect_(std::move(other.disconnect_)),
      sink_(std::move(other.sink_)),
      active_(other.active_) {
  other.active_ = false;
}

ConnectionTracker& ConnectionTracker::operator=(ConnectionTracker&& other) noexcept {
  if (this != &other) {
    Reset();
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
  put_in_server_.Restore();
  fully_connect_.Restore();
  disconnect_.Restore();
  sink_.reset();
  active_ = false;
}

void ConnectionTracker::DispatchPutInServer(int32_t slot, uint64_t xuid, bool is_bot,
                                            const char* name, const EngineCallback& original,
                                            std::shared_ptr<ConnectionEventSink> sink) {
  // These are connection receipts, not frame diagnostics. The server operator
  // needs every join, including a full roster and later reconnects.
  if (InteropTraceEnabled()) {
    std::fprintf(stderr, "[modlock] interop trace: ClientPutInServer slot %d xuid %llu\n", slot,
                 static_cast<unsigned long long>(xuid));
    std::fflush(stderr);
  }
  if (original) {
    original();
  }
  ApplyPutInServer(slot, xuid, is_bot, name);
  if (ValidSlot(slot) && sink != nullptr) {
    sink->OnConnected(slot, xuid, is_bot, name);
  }
}

void ConnectionTracker::DispatchFullyConnect(int32_t slot, const EngineCallback& original) {
  // Run the engine's call, noting the client before and after.
  const auto before = StateForSlot(slot);
  if (original) {
    original();
  }

  // Mark it fully connected only if the same client stayed.
  const auto after = StateForSlot(slot);
  if (!before.occupied || !after.occupied || before.generation != after.generation) {
    return;
  }
  g_fully_connected[slot].store(true, std::memory_order_release);
  g_revision.fetch_add(1, std::memory_order_release);
  if (InteropTraceEnabled()) {
    std::fprintf(stderr, "[modlock] interop trace: ClientFullyConnect slot %d\n", slot);
    std::fflush(stderr);
  }
}

void ConnectionTracker::DispatchDisconnect(int32_t slot, const EngineCallback& original,
                                           std::shared_ptr<ConnectionEventSink> sink) {
  // Trace the disconnect.
  if (InteropTraceEnabled()) {
    std::fprintf(stderr, "[modlock] interop trace: ClientDisconnect slot %d\n", slot);
    std::fflush(stderr);
  }

  // A slot with no client runs the engine call, clearing the slot unless a client arrived
  // meanwhile.
  const SlotState state = StateForSlot(slot);
  if (!state.occupied || !ValidSlot(slot)) {
    const uint64_t generation =
        ValidSlot(slot) ? g_generation[slot].load(std::memory_order_acquire) : 0;
    if (original) {
      original();
    }
    if (!ValidSlot(slot) || g_generation[slot].load(std::memory_order_acquire) == generation) {
      ApplyDisconnect(slot);
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

  // Tell the sink, then run the engine's call.
  const uint64_t generation = g_generation[slot].load(std::memory_order_acquire);
  if (sink != nullptr) {
    sink->OnDisconnecting(slot, state.xuid);
  }
  if (original) {
    original();
  }

  // A reconnect during the sink callback is a new generation. Do not erase
  // that replacement client's occupied state when the old disconnect returns.
  if (g_generation[slot].load(std::memory_order_acquire) == generation) {
    ApplyDisconnect(slot);
    g_disconnect_claimed[slot].store(false, std::memory_order_release);
  }
}

void ConnectionTracker::ApplyPutInServer(int32_t slot, uint64_t xuid, bool is_bot,
                                         const char* name) {
  // Record the client in its slot.
  if (!ValidSlot(slot)) {
    return;
  }
  g_xuid[slot].store(xuid, std::memory_order_relaxed);
  g_is_bot[slot].store(is_bot, std::memory_order_relaxed);
  g_fully_connected[slot].store(is_bot, std::memory_order_relaxed);
  g_name[slot] = (name != nullptr) ? std::string(name) : std::string();
  g_generation[slot].fetch_add(1, std::memory_order_acq_rel);

  // A reconnect starts a new disconnect lifecycle, even when it occurs from
  // the previous generation's OnDisconnecting callback. Publish this claim
  // reset before occupied=true so a new disconnect cannot miss the generation.
  g_disconnect_claimed[slot].store(false, std::memory_order_release);
  g_occupied[slot].store(true, std::memory_order_release);
  g_revision.fetch_add(1, std::memory_order_release);
}

void ConnectionTracker::ApplyDisconnect(int32_t slot) {
  if (!ValidSlot(slot)) {
    return;
  }
  g_occupied[slot].store(false, std::memory_order_release);
  g_revision.fetch_add(1, std::memory_order_release);
}

uint64_t ConnectionTracker::Revision() {
  return g_revision.load(std::memory_order_acquire);
}

ConnectionTracker::SlotState ConnectionTracker::StateForSlot(int32_t slot) {
  if (!ValidSlot(slot) || !g_occupied[slot].load(std::memory_order_acquire)) {
    return SlotState{};
  }
  return SlotState{
      .occupied = true,
      .xuid = g_xuid[slot].load(std::memory_order_relaxed),
      .generation = static_cast<uint32_t>(g_generation[slot].load(std::memory_order_acquire)),
      .is_bot = g_is_bot[slot].load(std::memory_order_relaxed),
      .name = g_name[slot],
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

  // Hook each slot.
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
  return ConnectionTracker(std::move(*put_in_server), std::move(*fully_connect),
                           std::move(*disconnect), std::move(sink));
}

}  // namespace modlock::gameinterop
