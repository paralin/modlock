#include "quake/player_movement.h"

#include <optional>

#include "quake/collision_scene.h"
#include "quake/movement.h"

namespace modlock::quake {
namespace {

using gameinterop::MovementHook;

// kOriginHeight is the Quake origin's height above the feet, in Quake units.
constexpr float kOriginHeight = 24;

// kCommandSpeed is an always-run QuakeWorld client's forward and side speed.
// Movement clamps the combined wish speed to 320.
constexpr float kCommandSpeed = 400;

// kTickMilliseconds is Deadlock's simulation interval. A command's duration
// depends only on its own share of ticks, so the tick the server executes it
// at leaves the step unchanged.
constexpr double kTickMilliseconds = 1000.0 / 64;

// Steps is how many Quake moves share a command. QuakeWorld caps each move's
// air gain, so strafe gain grows with the move rate; competitive QuakeWorld
// runs 77 moves a second. Every fifth command takes two half moves, 76.8 a
// second, keyed by command number.
int Steps(int32_t command) {
  const auto at = [](int64_t number) {
    return number * 6 / 5;
  };
  const int64_t number = command < 0 ? 0 : command;
  return int(at(number + 1) - at(number));
}

float Milliseconds(const gameinterop::MovementCall& call) {
  const double ticks = call.ticks[1] - call.ticks[0];
  return static_cast<float>(ticks * (call.fractions[1] - call.fractions[0]) * kTickMilliseconds);
}

}  // namespace

std::expected<PlayerMovement, std::string> PlayerMovement::Create(
    std::span<const uint8_t> collision, float scale) {
  auto world = BspWorld::Decode(collision);
  if (!world) {
    return std::unexpected("Quake collision: " + world.error());
  }
  return PlayerMovement(std::move(*world), scale > 0 ? scale : 1);
}

void PlayerMovement::Land(uint32_t player, const MovementState& state,
                          std::span<const uint32_t> bodies, float fall) {
  const auto body = size_t(state.ground_entity);
  landings_.push_back({
      .player = player,
      .on = body > 0 && body <= bodies.size() ? bodies[body - 1] : 0,
      .speed = -fall * scale_,
  });
}

bool PlayerMovement::Step(MovementCall& call) {
  // Only a walking pawn moves; read its state in movement units.
  if (call.move_type != MovementHook::kWalking) {
    return false;
  }
  MovementState state;
  for (size_t axis = 0; axis < 3; ++axis) {
    state.origin[axis] = call.origin[axis] / scale_;
    state.velocity[axis] = call.velocity[axis] / scale_;
  }
  state.origin[2] += kOriginHeight;
  auto& pawn = pawns_[call.services];
  state.jump_held = pawn.jump_held;

  // The command field holds the previously processed number.
  const int steps = Steps(call.command + 1);
  const MovementCommand command{
      .angles = call.angles,
      .forward = call.forward * kCommandSpeed,
      .side = -call.left * kCommandSpeed,
      .milliseconds = Milliseconds(call) / float(steps),
      .jump = (call.buttons[0] & kJumpButtons) != 0,
  };

  // Other players are solid boxes; entity n names players_[n - 1].
  std::optional<uint32_t> self;
  std::vector<CollisionBody> bodies;
  std::vector<uint32_t> handles;
  for (const auto& player : players_) {
    // Skip the mover, keeping its handle; every other player is a body.
    if (call.pawn && player.pawn == call.pawn) {
      self = player.handle;
      continue;
    }
    CollisionBody body{.entity = int(bodies.size()) + 1};
    for (size_t axis = 0; axis < 3; ++axis) {
      body.origin[axis] = player.feet[axis] / scale_;
    }
    body.origin[2] += kOriginHeight;
    bodies.push_back(body);
    handles.push_back(player.handle);
  }
  const CollisionScene scene(collision_, bodies);

  // A landing compares the ground with the fall speed before this move.
  const float fall = state.velocity[2];
  auto next = state;
  for (int step = 0; step < steps; ++step) {
    next = Move(next, command, scene).state;
  }
  pawn.jump_held = next.jump_held;
  const bool grounded = next.ground_entity >= 0;
  if (grounded && !pawn.grounded && self && fall < 0) {
    Land(*self, next, handles, fall);
  }
  pawn.grounded = grounded;

  // Standing on a player is grounded; the pawn keeps the world as its ground.
  if (call.ground_entity != MovementHook::kNoGround) {
    world_ = call.ground_entity;
  }
  if (next.ground_entity < 0) {
    call.ground_entity = MovementHook::kNoGround;
  } else if (world_ != MovementHook::kNoGround) {
    call.ground_entity = world_;
  }

  // Return the move in the game's units.
  for (size_t axis = 0; axis < 3; ++axis) {
    call.origin[axis] = next.origin[axis] * scale_;
    call.velocity[axis] = next.velocity[axis] * scale_;
  }
  call.origin[2] -= kOriginHeight * scale_;
  return true;
}

}  // namespace modlock::quake
