#include "modlock/gameinterop/engine_server.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/startup_server_probe.h"
#include "modlock/gameinterop/virtual_slots.h"

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

// ReleaseReservation clears the engine's reconnect reservation on slot and
// reports whether the release call ran.
//
// The game can ask the network game server to hold a human's client in its
// slot after they disconnect, so the same player gets it back on reconnect.
// The client then keeps its address and Steam ID, the slot accepts only that
// player again, and every other joiner is refused as server full once all
// slots are held this way. Disconnect frees the slot only when the client is
// not reserved, so clearing the reservation first makes a disconnect free the
// slot, and a second disconnect of a slot already held for a departed player
// frees it too. Fake clients carry a reservation the call leaves unchanged.
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

// The server's client list is a CUtlVector of CServerSideClient pointers
// indexed by slot, its count at +0x268 and its elements at +0x270; the
// set-client-released signature reads both. The row fields are those status
// prints (engine2 build 6864: the row printer at RVA 0xc2ee0 and its reserved
// test at RVA 0xc13d0): the signon state at +0x64, which status names active
// at 6, spawning from 3, connecting at 2, and below that reserved or
// challenging; the reservation at +0x9c, which with a set address makes the
// row "reserved"; and the stored address at +0xcc, a 0x20-byte record whose
// kind is the word at its +0x1c, 0 meaning none set. These offsets are not
// signature-checked, so a game update that moves them garbles this log line
// and nothing else.
constexpr size_t kClientCountOffset = 0x268;
constexpr size_t kClientsOffset = 0x270;
constexpr size_t kSignonStateOffset = 0x64;
constexpr size_t kReservationOffset = 0x9c;
constexpr size_t kAddressOffset = 0xcc;
constexpr size_t kAddressSize = 0x20;
constexpr size_t kAddressKindOffset = 0x1c;

// ClientRow is a read-only copy of one client row's fields status prints.
struct ClientRow {
  bool present = false;
  int32_t state = 0;
  int32_t reservation = 0;
  unsigned char address[kAddressSize] = {};
};

// ReadClientRow copies slot's row from server's client list. It reads and
// never writes, and it is called only around a server-initiated disconnect.
ClientRow ReadClientRow(const void* server, int32_t slot) {
  ClientRow row;
  const auto* base = static_cast<const unsigned char*>(server);
  if (!base || slot < 0) {
    return row;
  }
  int32_t count = 0;
  const unsigned char* const* clients = nullptr;
  std::memcpy(&count, base + kClientCountOffset, sizeof(count));
  std::memcpy(&clients, base + kClientsOffset, sizeof(clients));
  if (slot >= count || !clients || !clients[slot]) {
    return row;
  }
  const auto* client = clients[slot];
  row.present = true;
  std::memcpy(&row.state, client + kSignonStateOffset, sizeof(row.state));
  std::memcpy(&row.reservation, client + kReservationOffset, sizeof(row.reservation));
  std::memcpy(row.address, client + kAddressOffset, sizeof(row.address));
  return row;
}

// FormatClientRow writes row as "state=S(name) hold=H addr=K:<hex>".
std::string FormatClientRow(const ClientRow& row) {
  if (!row.present) {
    return "absent";
  }
  const char* name = row.state == 6         ? "active"
                     : row.state >= 3       ? "spawning"
                     : row.state >= 2       ? "connecting"
                     : row.reservation != 0 ? "held"
                                            : "challenging";
  uint32_t kind = 0;
  std::memcpy(&kind, row.address + kAddressKindOffset, sizeof(kind));
  char text[128];
  int used = std::snprintf(text, sizeof(text), "state=%d(%s) hold=%d addr=%u:", row.state, name,
                           row.reservation, kind);
  for (size_t i = 0; i < kAddressSize && used > 0 && size_t(used) + 2 < sizeof(text); ++i) {
    used += std::snprintf(text + used, sizeof(text) - used, "%02x", row.address[i]);
  }
  return text;
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

  // Release the slot's reservation, then disconnect. Without the release the
  // engine keeps a removed human's slot held for their reconnect. When the
  // release runs, log the row before the release and after the disconnect.
  void* server = NetworkGameServer();
  const ClientRow before = ReadClientRow(server, slot);
  const bool released = ReleaseReservation(server, slot);
  using DisconnectClientFn = void (*)(void* self, int32_t slot, int32_t reason, const char* text);
  reinterpret_cast<DisconnectClientFn>(table[*index])(instance_, slot, reason, internal_reason);
  if (released) {
    const ClientRow after = ReadClientRow(server, slot);
    std::fprintf(stderr, "[modlock] slot release: slot=%d before %s after %s\n", slot,
                 FormatClientRow(before).c_str(), FormatClientRow(after).c_str());
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
