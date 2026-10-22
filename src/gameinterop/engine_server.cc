#include "modlock/gameinterop/engine_server.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace modlock::gameinterop {
namespace {

// DispatchTableOf reads the vptr every MSVC object carries as its first word.
void** DispatchTableOf(void* instance) {
  return *static_cast<void***>(instance);
}

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

}  // namespace modlock::gameinterop
