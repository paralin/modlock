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
// depends only on its own share of ticks: the server executes each command at a
// later tick than the client predicted it, so absolute tick bounds must not
// influence the step.
constexpr double kTickMilliseconds = 1000.0 / 64;

// Steps is how many Quake moves share a command. QuakeWorld caps each move's
// air gain, so strafe gain grows with the move rate; competitive QuakeWorld
// runs 77 moves a second. Every fifth command takes two half moves, 76.8 a
// second, keyed by command number so the server and the predicting client
// split the same commands.
int Steps(int32_t command) {
  const auto at = [](int64_t number) { return number * 6 / 5; };
  const int64_t number = command < 0 ? 0 : command;
  return int(at(number + 1) - at(number));
}

float Milliseconds(const gameinterop::MovementCall& call) {
  const double ticks = call.ticks[1] - call.ticks[0];
  return static_cast<float>(ticks * (call.fractions[1] - call.fractions[0]) * kTickMilliseconds);
}

}  // namespace

std::expected<PlayerMovement, std::string> PlayerMovement::Create(
    Side side, std::span<const uint8_t> collision, float scale) {
  auto world = BspWorld::Decode(collision);
  if (!world) return std::unexpected("Quake collision: " + world.error());
  return PlayerMovement(side, std::move(*world), scale > 0 ? scale : 1);
}

bool PlayerMovement::JumpHeld(const MovementCall& call) const {
  if (side_ == Side::kServer) {
    const auto found = server_pawns_.find(call.services);
    return found != server_pawns_.end() && found->second.jump_held;
  }

  // A later subdivision of the same command continues from the earlier one.
  const int32_t previous_command = call.fractions[0] > 0 ? call.command : call.command - 1;
  const auto& previous = client_latches_[static_cast<uint8_t>(previous_command)];
  return previous.command == previous_command && previous.held;
}

void PlayerMovement::RetainJumpHeld(const MovementCall& call, bool held) {
  if (side_ == Side::kServer) {
    server_pawns_[call.services].jump_held = held;
    return;
  }
  client_latches_[static_cast<uint8_t>(call.command)] = {call.command, held};
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
  if (call.move_type != MovementHook::kWalking) return false;
  MovementState state;
  for (size_t axis = 0; axis < 3; ++axis) {
    state.origin[axis] = call.origin[axis] / scale_;
    state.velocity[axis] = call.velocity[axis] / scale_;
  }
  state.origin[2] += kOriginHeight;
  state.jump_held = JumpHeld(call);

  // The server's command field holds the previously processed number.
  const int32_t number = side_ == Side::kServer ? call.command + 1 : call.command;
  const int steps = Steps(number);
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
    if (call.pawn && player.pawn == call.pawn) {
      self = player.handle;
      continue;
    }
    CollisionBody body{.entity = int(bodies.size()) + 1};
    for (size_t axis = 0; axis < 3; ++axis) body.origin[axis] = player.feet[axis] / scale_;
    body.origin[2] += kOriginHeight;
    bodies.push_back(body);
    handles.push_back(player.handle);
  }
  const CollisionScene scene(collision_, bodies);

  // A landing compares the ground with the fall speed before this move.
  const float fall = state.velocity[2];
  auto next = state;
  for (int step = 0; step < steps; ++step) next = Move(next, command, scene).state;
  RetainJumpHeld(call, next.jump_held);
  if (side_ == Side::kServer) {
    auto& pawn = server_pawns_[call.services];
    const bool grounded = next.ground_entity >= 0;
    if (grounded && !pawn.grounded && self && fall < 0) Land(*self, next, handles, fall);
    pawn.grounded = grounded;
  }

  // Standing on a player is grounded; the pawn keeps the world as its ground.
  if (call.ground_entity != MovementHook::kNoGround) world_ = call.ground_entity;
  if (next.ground_entity < 0)
    call.ground_entity = MovementHook::kNoGround;
  else if (world_ != MovementHook::kNoGround)
    call.ground_entity = world_;

  for (size_t axis = 0; axis < 3; ++axis) {
    call.origin[axis] = next.origin[axis] * scale_;
    call.velocity[axis] = next.velocity[axis] * scale_;
  }
  call.origin[2] -= kOriginHeight * scale_;
  return true;
}

}  // namespace modlock::quake
