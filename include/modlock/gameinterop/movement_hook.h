#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "modlock/export.h"

namespace modlock::gameinterop {

// MovementCall is one native player movement step borrowed for its handler.
// Angles are degrees; input axes are -1..1; positions are feet-origin world
// units and velocities are world units per second. A handler may rewrite the
// angles and input axes as well as the buttons; the native step then runs on
// the rewritten command. Ticks and fractions bound
// the step's share of the command in the engine's own time base.
struct MovementCall {
  // services is the pawn's movement-services component; pawn is its owner,
  // or null when the component's owner cannot be verified.
  void* services = nullptr;
  void* pawn = nullptr;
  // move_type is the pawn's native MoveType_t; preparation freezes use kNone.
  uint8_t move_type = 0;
  // command is the client command number on the client. The server stores the
  // previously processed number and must not use it as a command identity.
  int32_t command = 0;
  std::array<float, 3> angles{};
  float forward = 0;
  float left = 0;
  float up = 0;
  // buttons holds the held, changed and scroll InButtonState masks. A handler
  // may rewrite them; the rest of this command's simulation sees the result.
  std::array<uint64_t, 3> buttons{};
  std::array<int32_t, 2> ticks{};
  std::array<float, 2> fractions{};
  // origin, velocity and ground_entity are inputs; a handler that replaces the
  // step writes its result into them. ground_entity is the pawn's supporting
  // entity handle, or kNoGround while airborne.
  std::array<float, 3> origin{};
  std::array<float, 3> velocity{};
  uint32_t ground_entity = 0;
};

// MovementHook detours the native ProcessMovement of one game module. The
// handler runs synchronously on that module's movement thread for every step,
// including client prediction replays. The native step always runs afterward,
// because client prediction depends on its per-command bookkeeping. Returning
// true then publishes the handler's origin, velocity and ground entity over the
// native motion. Publishing does not notify replication; the server handler
// notifies the pawn when its networked ground entity changes.
// One hook per module may exist; destruction removes the detour.
class MODLOCK_API MovementHook {
 public:
  enum class Module { kServer, kClient };
  using Handler = std::function<bool(MovementCall&)>;

  // kJumpButton is the native InButtonState bit for jump.
  static constexpr uint64_t kJumpButton = 1u << 1;
  // kNoGround is the invalid entity handle stored while a pawn is airborne.
  static constexpr uint32_t kNoGround = 0xffffffffu;
  // kWalking is the MoveType_t of a pawn under ordinary player movement.
  static constexpr uint8_t kWalking = 2;

  static std::expected<MovementHook, std::string> Install(Module module, Handler handler);
  MovementHook(MovementHook&&) noexcept;
  MovementHook& operator=(MovementHook&&) noexcept;
  ~MovementHook();
  MovementHook(const MovementHook&) = delete;
  MovementHook& operator=(const MovementHook&) = delete;

 private:
  struct Impl;
  explicit MovementHook(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
