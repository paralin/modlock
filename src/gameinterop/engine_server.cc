#include "modlock/gameinterop/engine_server.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/startup_server_probe.h"
#include "modlock/gameinterop/virtual_slots.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::gameinterop {
namespace {

// DispatchTableOf reads the vptr every MSVC object carries as its first word.
void** DispatchTableOf(void* instance) {
  return *static_cast<void***>(instance);
}

#if defined(_WIN32)
// NetworkGameServer returns the engine's CNetworkGameServer, or null between
// worlds or when its interface does not resolve.
void* NetworkGameServer() {
  const auto service = ResolveEngineInterface(L"engine2.dll", kNetworkServerServiceVersion);
  const auto game_server_slot = ResolveVirtualSlot("network-server-service.game-server");
  if (!service || !game_server_slot) {
    return nullptr;
  }
  auto** service_table = DispatchTableOf(*service);
  if (!service_table || !service_table[*game_server_slot]) {
    return nullptr;
  }
  return reinterpret_cast<void* (*)(void*)>(service_table[*game_server_slot])(*service);
}

// The server's client list is a CUtlVector of CServerSideClient pointers
// indexed by slot, its count at +0x268 and its elements at +0x270; the
// set-client-released signature reads both. The row fields below are those
// the engine reads to print status and to pick a slot for a joiner (engine2
// build 6864: the row printer at RVA 0xc2ee0, its reserved test at RVA
// 0xc13d0, the free-client allocator at RVA 0xab7a0, and the client
// disconnect at RVA 0xc1d30):
//
//   - +0x64, the signon state: 6 active, 3 to 5 spawning, 2 connecting, and 0
//     once the client has disconnected.
//   - +0x9c, the reservation: 1 while the game holds the slot for its
//     player's reconnect, 0 once released, and 2 for a fake client.
//   - +0xcc, the stored address, a 0x20-byte record. Its first 12 bytes are a
//     netadr_t whose first word is the netadr type (0 none, 3 IP), and the
//     word at its +0x1c is the record's kind, 0 when the record holds that
//     netadr_t.
//
// These offsets are not signature-checked. A game update that moves them
// garbles the slot release log line, and the address clear below refuses a
// row whose state, reservation and kind do not read as a released one.
constexpr size_t kClientCountOffset = 0x268;
constexpr size_t kClientsOffset = 0x270;
constexpr size_t kSignonStateOffset = 0x64;
constexpr size_t kReservationOffset = 0x9c;
constexpr size_t kAddressOffset = 0xcc;
constexpr size_t kAddressSize = 0x20;
constexpr size_t kAddressKindOffset = 0x1c;

// ClientAt returns slot's CServerSideClient from server's client list, or
// null when the slot is out of range or empty.
unsigned char* ClientAt(void* server, int32_t slot) {
  auto* base = static_cast<unsigned char*>(server);
  if (!base || slot < 0) {
    return nullptr;
  }
  int32_t count = 0;
  unsigned char** clients = nullptr;
  std::memcpy(&count, base + kClientCountOffset, sizeof(count));
  std::memcpy(&clients, base + kClientsOffset, sizeof(clients));
  if (slot >= count || !clients) {
    return nullptr;
  }
  return clients[slot];
}

// ClientRow is a read-only copy of one client row's fields status prints.
struct ClientRow {
  bool present = false;
  int32_t state = 0;
  int32_t reservation = 0;
  unsigned char address[kAddressSize] = {};

  // type is the stored netadr_t's type, 0 when none is set.
  uint32_t type() const {
    uint32_t value = 0;
    std::memcpy(&value, address, sizeof(value));
    return value;
  }

  // kind is the stored address record's kind, 0 when it holds a netadr_t.
  uint32_t kind() const {
    uint32_t value = 0;
    std::memcpy(&value, address + kAddressKindOffset, sizeof(value));
    return value;
  }

  // HasAddress reports whether the row remembers a player's address. The
  // free-client allocator hands a stranger only a row without one; a row
  // with one goes only to the player whose address it matches.
  bool HasAddress() const { return type() != 0 || kind() != 0; }
};

// ReadClientRow copies slot's row from server's client list. It never writes.
ClientRow ReadClientRow(void* server, int32_t slot) {
  ClientRow row;
  const unsigned char* client = ClientAt(server, slot);
  if (!client) {
    return row;
  }
  row.present = true;
  std::memcpy(&row.state, client + kSignonStateOffset, sizeof(row.state));
  std::memcpy(&row.reservation, client + kReservationOffset, sizeof(row.reservation));
  std::memcpy(row.address, client + kAddressOffset, sizeof(row.address));
  return row;
}

// FormatClientRow writes row as
// "state=S(name) hold=H addr=type:T,kind:K,<hex>", where hex is the 0x20
// bytes of the stored address record, or "addr=none" when it holds none.
std::string FormatClientRow(const ClientRow& row) {
  if (!row.present) {
    return "absent";
  }
  const char* name = row.state == 6         ? "active"
                     : row.state >= 3       ? "spawning"
                     : row.state >= 2       ? "connecting"
                     : row.reservation != 0 ? "held"
                                            : "challenging";
  char text[160];
  int used =
      std::snprintf(text, sizeof(text), "state=%d(%s) hold=%d ", row.state, name, row.reservation);
  if (!row.HasAddress()) {
    std::snprintf(text + used, sizeof(text) - used, "addr=none");
    return text;
  }
  used += std::snprintf(text + used, sizeof(text) - used, "addr=type:%u,kind:%u,", row.type(),
                        row.kind());
  for (size_t i = 0; i < kAddressSize && used > 0 && size_t(used) + 2 < sizeof(text); ++i) {
    used += std::snprintf(text + used, sizeof(text) - used, "%02x", row.address[i]);
  }
  return text;
}

// ReleaseReservation clears the engine's reconnect reservation on slot and
// reports whether the release call ran. Fake clients carry a reservation the
// call leaves unchanged.
bool ReleaseReservation(void* server, int32_t slot) {
  const auto release_slot = ResolveVirtualSlot("network-game-server.set-client-released");
  if (!server || !release_slot) {
    return false;
  }
  auto** server_table = DispatchTableOf(server);
  if (!server_table || !server_table[*release_slot]) {
    return false;
  }

  // Release the slot. The engine checks the slot against its client list.
  using SetClientReleasedFn = void (*)(void* self, int32_t slot, bool released);
  reinterpret_cast<SetClientReleasedFn>(server_table[*release_slot])(server, slot, true);
  return true;
}

// ForgetAddress clears the stored address of slot's disconnected, released
// client, returning the row to the state of one never used, and reports
// whether it cleared one.
//
// The engine's client disconnect (RVA 0xc1d30) clears the address only when
// the client is not reserved at that moment, and the game reserves a
// departing human's slot from inside that disconnect, so the address stays.
// A disconnect of a client already at signon state 0 does nothing, so no
// later disconnect clears it either. The free-client allocator (RVA 0xab7a0)
// reuses a row with an address only for the joiner at that address, so each
// departed player's row is lost to everyone else, and once the list is full
// every new joiner is refused as server full. ForgetAddress performs the
// clear the disconnect skipped, field for field: netadr_t::Clear on the
// netadr_t, then zero the rest of the record and its kind.
//
// It touches only a row at signon state 0 with no reservation and a kind of
// 0, which is the row the disconnect's own clear would have reset, and it
// leaves the row unchanged when tier0.dll's netadr_t::Clear does not
// resolve.
bool ForgetAddress(void* server, int32_t slot) {
  const ClientRow row = ReadClientRow(server, slot);
  if (!row.present || row.state != 0 || row.reservation != 0 || row.kind() != 0 ||
      !row.HasAddress()) {
    return false;
  }
  HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
  if (tier0 == nullptr) {
    return false;
  }
  using NetAdrClearFn = void (*)(void* self);
  auto clear = reinterpret_cast<NetAdrClearFn>(GetProcAddress(tier0, "?Clear@netadr_t@@QEAAXXZ"));
  if (clear == nullptr) {
    return false;
  }
  constexpr size_t kNetAdrSize = 0xc;
  unsigned char* address = ClientAt(server, slot) + kAddressOffset;
  clear(address);
  std::memset(address + kNetAdrSize, 0, kAddressSize - kNetAdrSize);
  return true;
}
#endif

}  // namespace

std::expected<EngineServer, std::string> EngineServer::Resolve() {
  const auto instance = ResolveEngineInterface(L"engine2.dll", kEngineToServerVersion);
  if (!instance) {
    return std::unexpected(instance.error());
  }
  return Bind(*instance);
}

std::expected<EngineServer, std::string> EngineServer::Bind(void* instance) {
  // Resolve the command slot from the instance's table.
  if (!instance) {
    return std::unexpected("engine server interface is null");
  }
  const auto slot = ResolveVirtualSlot("engine-server.server-command");
  if (!slot) {
    return std::unexpected(slot.error());
  }
  auto** table = DispatchTableOf(instance);
  if (!table || !table[*slot]) {
    return std::unexpected("engine server command slot is unavailable");
  }

  // Keep the instance and slot.
  EngineServer server;
  server.instance_ = instance;
  server.command_slot_ = *slot;
  return server;
}

std::expected<EngineServer::SimulationClock, std::string> EngineServer::ReadClock() const {
  // Find the globals slot in the instance's table.
  if (!instance_) {
    return std::unexpected("engine server interface is unavailable");
  }
  const auto slot = ResolveVirtualSlot("engine-server.globals");
  if (!slot) {
    return std::unexpected(slot.error());
  }
  auto** table = DispatchTableOf(instance_);
  if (!table || !table[*slot]) {
    return std::unexpected("server globals slot is unavailable");
  }

  // Call it.
  using GetGlobals = const void* (*)(void*);
  const auto* globals =
      static_cast<const unsigned char*>(reinterpret_cast<GetGlobals>(table[*slot])(instance_));
  if (!globals) {
    return std::unexpected("server simulation globals are unavailable");
  }

  // CGlobalVarsBase x64 layout from the pinned SDK's globalvars_base.h:
  // curtime +0x30, tickcount +0x44, interval_per_tick +0x54. The callback's
  // engine frame owns the globals storage and all three values.
  SimulationClock clock;
  std::memcpy(&clock.current_time, globals + 0x30, sizeof(clock.current_time));
  std::memcpy(&clock.tick, globals + 0x44, sizeof(clock.tick));
  std::memcpy(&clock.interval, globals + 0x54, sizeof(clock.interval));
  if (!std::isfinite(clock.current_time) || clock.current_time < 0 || clock.tick < 0 ||
      !std::isfinite(clock.interval) || clock.interval <= 0 || clock.interval > 1) {
    return std::unexpected("invalid server simulation clock");
  }

  // Reject an inconsistent ABI read, allowing two simulation ticks and float
  // rounding at long uptimes. This is not a replacement or synthesized clock.
  const double tolerance =
      std::max(double(clock.interval) * 2,
               double(clock.current_time) * std::numeric_limits<float>::epsilon() * 2);
  if (std::abs(double(clock.current_time) - double(clock.tick) * clock.interval) > tolerance) {
    return std::unexpected("server time and tick do not agree");
  }
  return clock;
}

bool EngineServer::ServerCommand(const char* command) const {
#if defined(_WIN32)
  // Call the command slot.
  if (instance_ == nullptr || command == nullptr) {
    return false;
  }
  using ServerCommandFn = void (*)(void* self, const char* str);
  void** table = DispatchTableOf(instance_);
  if (table == nullptr || table[command_slot_] == nullptr) {
    return false;
  }
  reinterpret_cast<ServerCommandFn>(table[command_slot_])(instance_, command);
  return true;
#else
  (void)command;
  return false;
#endif
}

bool EngineServer::DisconnectClient(int32_t slot, int32_t reason,
                                    const char* internal_reason) const {
#if defined(_WIN32)
  // Find the disconnect slot in the instance's table.
  if (instance_ == nullptr) {
    return false;
  }
  const auto index = ResolveVirtualSlot("engine-server.disconnect-client");
  if (!index) {
    return false;
  }
  void** table = DispatchTableOf(instance_);
  if (table == nullptr || table[*index] == nullptr) {
    return false;
  }

  // End the slot's seat and free the slot for any joiner. Releasing the
  // reservation before the disconnect lets the engine's disconnect clear the
  // address itself; the game reserves the slot again from inside that
  // disconnect, so release it once more afterward and clear the address the
  // engine kept. A slot whose client disconnected earlier takes the same two
  // steps. Log the row before and after.
  ConnectionTracker::Release(slot, reason);
  void* server = NetworkGameServer();
  const ClientRow before = ReadClientRow(server, slot);
  const bool released = ReleaseReservation(server, slot);
  using DisconnectClientFn = void (*)(void* self, int32_t slot, int32_t reason, const char* text);
  reinterpret_cast<DisconnectClientFn>(table[*index])(instance_, slot, reason, internal_reason);
  if (released) {
    ReleaseReservation(server, slot);
    const bool forgot = ForgetAddress(server, slot);
    const ClientRow after = ReadClientRow(server, slot);
    std::fprintf(stderr, "[modlock] slot release: slot=%d before %s after %s%s\n", slot,
                 FormatClientRow(before).c_str(), FormatClientRow(after).c_str(),
                 forgot ? " address cleared" : "");
    std::fflush(stderr);
  }
  return true;
#else
  (void)slot;
  (void)reason;
  (void)internal_reason;
  return false;
#endif
}

}  // namespace modlock::gameinterop
