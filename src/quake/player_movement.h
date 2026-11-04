#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "modlock/gameinterop/movement_hook.h"
#include "quake/bsp_world.h"
#include "quake/movement.h"

namespace modlock::quake {

// PlayerMovement replaces the server's movement solver with QuakeWorld's.
// Coordinates convert at scale game units per Quake unit, and Quake's origin
// sits 24 Quake units above the game's feet.
class PlayerMovement {
 public:
  using MovementCall = gameinterop::MovementCall;

  // kMantleButton is IN_MANTLE, the bit Deadlock's jump key (the Mantle
  // action, +in_mantle) sets; the console +jump sets IN_JUMP instead. Both
  // jump.
  static constexpr uint64_t kMantleButton = uint64_t{1} << 48;
  static constexpr uint64_t kJumpButtons = gameinterop::MovementHook::kJumpButton | kMantleButton;

  // Create decodes the collision, a version 29 BSP; scale is game units per
  // Quake unit.
  static std::expected<PlayerMovement, std::string> Create(std::span<const uint8_t> collision,
                                                           float scale);

  // Step replaces the native solver for a walking pawn and returns false for
  // every other move type, such as a preparation freeze, so native movement
  // keeps owning it. A grounded pawn stands on the world entity once its
  // handle is known.
  bool Step(MovementCall& call);

  // SetWorld supplies the world entity handle before any pawn has stood on it.
  void SetWorld(uint32_t handle) { world_ = handle; }

  // Player is another pawn's feet position in game units. QuakeWorld players
  // are solid boxes to each other: one can stand on another's head.
  struct Player {
    const void* pawn = nullptr;
    uint32_t handle = 0;
    std::array<float, 3> feet{};
  };

  // SetPlayers supplies every live pawn for this frame's steps; a step ignores
  // its own pawn.
  void SetPlayers(std::vector<Player> players) { players_ = std::move(players); }

  // Landing is one pawn ending a fall on the ground or on another player. On
  // is the handle of the player landed on, or zero for the world; speed is
  // the downward speed before landing in game units per second. Only pawns in
  // SetPlayers record landings.
  struct Landing {
    uint32_t player = 0;
    uint32_t on = 0;
    float speed = 0;
  };

  std::vector<Landing> TakeLandings() { return std::exchange(landings_, {}); }

 private:
  PlayerMovement(BspWorld world, float scale) : collision_(std::move(world)), scale_(scale) {}

  // Land records a landing on the ground state's entity; bodies[n - 1] is
  // the handle of body n, and fall is the vertical speed before the move in
  // Quake units.
  void Land(uint32_t player, const MovementState& state, std::span<const uint32_t> bodies,
            float fall);

  // Pawn is one pawn's state in command order: QuakeWorld's jump latch, which
  // a jump sets and releasing the button clears, and whether its last step
  // ended on the ground, so a landing is reported once.
  struct Pawn {
    bool jump_held = false;
    bool grounded = false;
  };

  BspWorld collision_;
  float scale_;
  uint32_t world_ = gameinterop::MovementHook::kNoGround;
  std::unordered_map<const void*, Pawn> pawns_;
  std::vector<Player> players_;
  std::vector<Landing> landings_;
};

}  // namespace modlock::quake
