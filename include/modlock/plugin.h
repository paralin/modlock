#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "modlock/export.h"
#include "proto/modlock/types.pb.h"

namespace modlock {

class Vec3;
class EulerAngles;

// PluginInterfaceVersion is the ABI contract this header declares. A plugin
// built against a different value is rejected at registration.
inline constexpr uint32_t PluginInterfaceVersion = 6;

// PawnSample is one pawn position observed at a host tick, in world units.
struct PawnSample {
  // slot identifies the observing player controller.
  int32_t slot = 0;
  // x is the east-west world coordinate.
  double x = 0;
  // y is the north-south world coordinate.
  double y = 0;
  // z is the vertical world coordinate.
  double z = 0;
  // steam_id identifies the connected player for this observation.
  uint64_t steam_id = 0;
  // session_generation changes after disconnect or slot identity replacement.
  uint32_t session_generation = 0;
  // Stamina is absent unless native consumption observation is uninterrupted.
  std::optional<StaminaEvidence> stamina;
  // hero_id identifies the native hero observed for this frame.
  uint32_t hero_id = 0;
  // pawn_handle identifies the current native pawn, including its serial number.
  uint32_t pawn_handle = 0;
};

// PawnControl is the host-supplied mutation seam for addressed pawns.
class MODLOCK_API PawnControl {
 public:
  virtual ~PawnControl() = default;

  // TeleportToPosition moves one pawn when the game interop surface permits it.
  virtual bool TeleportToPosition(int32_t slot, const Vec3& position) = 0;

  // ResetToPose begins a fresh attempt at rest and sets the owning client's view.
  // Unsupported hosts refuse the reset instead of silently dropping the view.
  virtual bool ResetToPose(int32_t, const Vec3&, const EulerAngles&) { return false; }
};

// TickContext carries the server clock, observed pawn positions, and an
// optional frame-scoped pawn mutation seam for a tick. The host retains no
// references after the tick returns.
struct TickContext {
  // tick identifies the server frame.
  uint64_t tick = 0;
  // time_seconds is the game clock at this frame.
  double time_seconds = 0;
  // pawns contains the positions observed at this frame.
  std::vector<PawnSample> pawns;
  // pawn_control mutates addressed pawns, or is null when game interop cannot.
  PawnControl* pawn_control = nullptr;
};

// Plugin is the contract every modlock plugin implements. Methods default to
// no-ops so plugins implement only what they use; the host drives the full
// sequence Load -> Start -> Tick -> Stop and never calls methods after Stop.
//
// The interface carries no data members and destroys virtually: plugins may
// own arbitrary state behind it.
class MODLOCK_API Plugin {
 public:
  virtual ~Plugin() = default;

  // InterfaceVersion returns the ABI version the plugin was built against.
  [[nodiscard]] virtual uint32_t InterfaceVersion() const = 0;

  // Name returns a stable, human-readable plugin identifier.
  [[nodiscard]] virtual const char* Name() const = 0;

  // Load runs once before Start. Return false to refuse loading; the host
  // drops the plugin and reports the failure.
  [[nodiscard]] virtual bool Load() { return true; }

  // Start begins the plugin's active lifetime. Return false to fail startup.
  [[nodiscard]] virtual bool Start() { return true; }

  // Tick runs once per frame while the plugin is started.
  virtual void Tick() {}

  // Stop releases all resources acquired by Load and Start, including partial
  // failure. It runs once after loading begins; Tick never runs after it.
  virtual void Stop() {}

  // ExitCode reports a plugin's terminal process result after Stop, when its
  // operation needs to override a successful engine exit.
  virtual std::optional<int> ExitCode() const { return std::nullopt; }
};

// TickContextPlugin is an optional host extension for plugins that consume
// clock and pawn positions without changing the base Plugin ABI.
class MODLOCK_API TickContextPlugin {
 public:
  virtual ~TickContextPlugin() = default;

  // Tick consumes one frame with the positions supplied by the host.
  virtual void Tick(const TickContext& context) = 0;
};

}  // namespace modlock
