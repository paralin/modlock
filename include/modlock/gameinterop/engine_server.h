#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "modlock/export.h"

namespace modlock::gameinterop {

// EngineToServerVersion is engine2.dll's IVEngineServer2 interface version
// (sourcesdk public/interfaces/interfaces.h:
// SOURCE2ENGINETOSERVER_INTERFACE_VERSION).
inline constexpr char kEngineToServerVersion[] = "Source2EngineToServer001";

// kDisconnectKicked is NETWORK_DISCONNECT_KICKED, the reason the console's kick
// gives.
inline constexpr int32_t kDisconnectKicked = 39;

// EngineServer is a resolved live IVEngineServer2 instance. It issues console
// commands through the engine's own parser - identical to typing them at the
// server console - so automated setup rides exactly the path an operator would.
class MODLOCK_API EngineServer {
 public:
  // Resolve finds the mapped engine2.dll, requests the interface, and checks
  // that its dispatch table carries the ServerCommand slot. Failure names the
  // missing piece.
  static std::expected<EngineServer, std::string> Resolve();
  // Binds the factory's borrowed Source2EngineToServer001 instance. The engine
  // owns it; callers discard this view before unloading engine2.dll.
  static std::expected<EngineServer, std::string> Bind(void* instance);

  struct SimulationClock {
    float current_time;
    int32_t tick;
    float interval;
  };

  // Reads GetServerGlobals during the engine frame. Returned values are copied;
  // no globals pointer survives the call. Pauses follow simulation time.
  [[nodiscard]] std::expected<SimulationClock, std::string> ReadClock() const;

  // ServerCommand issues one command line through the engine's command parser
  // (IVEngineServer2::ServerCommand, sourcesdk public/eiface.h).
  // Returns false when the interface or command slot is unavailable.
  [[nodiscard]] bool ServerCommand(const char* command) const;

  // DisconnectClient ends the connection in slot with reason, an
  // ENetworkDisconnectionReason (network_connection.proto), and
  // internal_reason, the text the server logs
  // (IVEngineServer2::DisconnectClient). A client the engine is processing
  // leaves once that work ends, so the slot can stay held after the call.
  // Returns false when the interface or its slot is unavailable.
  [[nodiscard]] bool DisconnectClient(int32_t slot, int32_t reason,
                                      const char* internal_reason) const;

 private:
  void* instance_ = nullptr;
  // command_slot_ is ServerCommand's resolved dispatch table index.
  size_t command_slot_ = 0;
};

}  // namespace modlock::gameinterop
