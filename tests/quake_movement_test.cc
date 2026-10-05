#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "quake/bsp_world.h"
#include "quake/movement.h"
#include "quake/player_movement.h"

namespace modlock::quake {
namespace {

using gameinterop::MovementCall;
using gameinterop::MovementHook;

// kHalfWidth and kHeight bound the test room in Quake units: walls at x and y
// of plus or minus kHalfWidth, the floor at zero and the ceiling at kHeight.
constexpr float kHalfWidth = 288;
constexpr float kHeight = 2048;

// kScale is the game units per Quake unit the movement tests convert with.
constexpr float kScale = 1.25f;

template <typename T>
void Put(std::vector<uint8_t>& bytes, size_t offset, T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// BoxRoom compiles a closed room as a version 29 BSP. Each hull is a chain of
// six axial planes; leaving through any one is solid. Hull one is the point
// hull grown by the standing player's box.
std::vector<uint8_t> BoxRoom() {
  struct Wall {
    int axis;
    float distance;
    bool outside_front;
  };
  const auto walls = [](const Vector& mins, const Vector& maxs) {
    std::vector<Wall> result;
    for (int axis = 0; axis < 3; ++axis) {
      result.push_back({axis, maxs[axis], true});
      result.push_back({axis, mins[axis], false});
    }
    return result;
  };
  const auto point = walls({-kHalfWidth, -kHalfWidth, 0}, {kHalfWidth, kHalfWidth, kHeight});
  const auto player = walls({-kHalfWidth + 16, -kHalfWidth + 16, 24},
                            {kHalfWidth - 16, kHalfWidth - 16, kHeight - 32});

  constexpr size_t kHeader = 4 + 15 * 8;
  const size_t planes = kHeader;
  const size_t nodes = planes + 12 * 20;
  const size_t clips = nodes + 6 * 24;
  const size_t leafs = clips + 6 * 8;
  const size_t models = leafs + 2 * 28;
  std::vector<uint8_t> bytes(models + 64);
  Put<int32_t>(bytes, 0, 29);
  const auto lump = [&](int index, size_t offset, size_t length) {
    Put<uint32_t>(bytes, 4 + index * 8, uint32_t(offset));
    Put<uint32_t>(bytes, 8 + index * 8, uint32_t(length));
  };
  lump(1, planes, 12 * 20);
  lump(5, nodes, 6 * 24);
  lump(9, clips, 6 * 8);
  lump(10, leafs, 2 * 28);
  lump(14, models, 64);

  // Hull zero's children name leaves: -1 is leaf zero, empty; -2 is leaf one,
  // solid. Hull one's children are contents, encoded the same way.
  constexpr int16_t kEmpty = -1;
  constexpr int16_t kSolid = -2;
  Put<int32_t>(bytes, leafs, -1);
  Put<int32_t>(bytes, leafs + 28, -2);
  for (int hull = 0; hull < 2; ++hull) {
    const auto& chain = hull == 0 ? point : player;
    for (int index = 0; index < 6; ++index) {
      const auto& wall = chain[index];
      const size_t plane = planes + (hull * 6 + index) * 20;
      Put<float>(bytes, plane + wall.axis * 4, 1);
      Put<float>(bytes, plane + 12, wall.distance);
      Put<int32_t>(bytes, plane + 16, wall.axis);
      const int16_t inside = index == 5 ? kEmpty : int16_t(index + 1);
      const size_t node = hull == 0 ? nodes + index * 24 : clips + index * 8;
      Put<int32_t>(bytes, node, hull * 6 + index);
      Put<int16_t>(bytes, node + 4, wall.outside_front ? kSolid : inside);
      Put<int16_t>(bytes, node + 6, wall.outside_front ? inside : kSolid);
    }
  }
  return bytes;
}

class EmptyWorld final : public CollisionWorld {
 public:
  Trace Sweep(const Vector&, const Vector& end, const Vector&, const Vector&) const override {
    return {.end = end};
  }
  Contents At(const Vector&) const override { return Contents::kEmpty; }
};

TEST(QuakeCollision, CompiledRoomRetainsPlayerClearanceAndSolidContents) {
  const auto world = BspWorld::Decode(BoxRoom());
  ASSERT_TRUE(world) << world.error();
  EXPECT_EQ(world->At({0, 0, -8}), Contents::kSolid);
  EXPECT_EQ(world->At({0, 0, 100}), Contents::kEmpty);
  const auto trace = world->Sweep({0, 0, 100}, {0, 0, 0}, kPlayerMins, kPlayerMaxs);
  EXPECT_EQ(trace.normal, (Vector{0, 0, 1}));
  EXPECT_FLOAT_EQ(trace.end[2], 24.03125f);
  EXPECT_EQ(trace.entity, 0);
  EXPECT_FALSE(trace.start_solid);
}

TEST(QuakeCollision, RejectsTruncatedAndCyclicCollisionDataBeforeMovement) {
  auto bytes = BoxRoom();
  EXPECT_FALSE(BspWorld::Decode(std::span(bytes).first(124)));
  int32_t models = 0;
  int32_t clips = 0;
  int32_t root = 0;
  std::memcpy(&models, bytes.data() + 4 + 14 * 8, sizeof(models));
  std::memcpy(&clips, bytes.data() + 4 + 9 * 8, sizeof(clips));
  std::memcpy(&root, bytes.data() + models + 40, sizeof(root));
  const auto child = static_cast<int16_t>(root);
  std::memcpy(bytes.data() + clips + root * 8 + 4, &child, sizeof(child));
  EXPECT_FALSE(BspWorld::Decode(bytes));
}

TEST(QuakeMovement, AirProjectionCapsAccelerationWithoutCappingRetainedSpeed) {
  const auto next = Move({.origin = {0, 0, 200}, .velocity = {400, 0, 0}},
                         {.side = -320, .milliseconds = 13}, EmptyWorld{})
                        .state;
  EXPECT_FLOAT_EQ(next.velocity[0], 400);
  EXPECT_FLOAT_EQ(next.velocity[1], 30);
  EXPECT_FLOAT_EQ(next.velocity[2], -10.4f);
}

TEST(QuakeMovement, LongOddCommandUsesReferenceSubdivision) {
  const MovementState initial{.origin = {0, 0, 200}, .velocity = {400, 0, 0}};
  const MovementCommand half{.forward = 320, .milliseconds = 25};
  const auto first = Move(initial, half, EmptyWorld{}).state;
  const auto twice = Move(first, half, EmptyWorld{}).state;
  const auto split = Move(initial, {.forward = 320, .milliseconds = 51}, EmptyWorld{}).state;
  EXPECT_EQ(split.origin, twice.origin);
  EXPECT_EQ(split.velocity, twice.velocity);
}

PlayerMovement Create(PlayerMovement::Side side) {
  auto movement = PlayerMovement::Create(side, BoxRoom(), kScale);
  EXPECT_TRUE(movement) << movement.error();
  return std::move(*movement);
}

// Command builds one whole-tick client command.
MovementCall Command(int32_t number, float forward, bool jump, float yaw = 0) {
  return MovementCall{.move_type = MovementHook::kWalking,
                      .command = number,
                      .angles = {0, yaw, 0},
                      .forward = forward,
                      .buttons = {jump ? MovementHook::kJumpButton : 0, 0, 0},
                      .ticks = {number, number + 1},
                      .fractions = {0, 1}};
}

// Simulate applies commands in order, carrying the pawn's motion and ground
// like the engine.
std::vector<MovementCall> Simulate(PlayerMovement& movement, std::vector<MovementCall> commands,
                                   std::array<float, 3> origin, std::array<float, 3> velocity,
                                   uint32_t ground = 0) {
  for (auto& call : commands) {
    call.origin = origin;
    call.velocity = velocity;
    call.ground_entity = ground;
    EXPECT_TRUE(movement.Step(call));
    origin = call.origin;
    velocity = call.velocity;
    ground = call.ground_entity;
  }
  return commands;
}

float Speed(const MovementCall& call) { return std::hypot(call.velocity[0], call.velocity[1]); }

TEST(PlayerMovement, RunsAtQuakeSpeedOnTheFloor) {
  auto movement = Create(PlayerMovement::Side::kServer);
  std::vector<MovementCall> commands;
  for (int32_t number = 1; number <= 64; ++number) commands.push_back(Command(number, 1, false));
  const auto moved = Simulate(movement, commands, {-200, 0, 0}, {});
  EXPECT_NEAR(moved.back().origin[2], 0, 0.05f);
  EXPECT_EQ(moved.back().ground_entity, 0u) << "a grounded pawn keeps the learned world handle";
  EXPECT_NEAR(Speed(moved.back()), 400, 0.01f);
  EXPECT_GT(moved.back().origin[0], 100);
}

TEST(PlayerMovement, ServerAndClientSplitTheSameCommandsAt77Hz) {
  // The server's command field is the previous number, so server command n - 1
  // is client command n; both must produce the same state, strafe-hopping.
  auto server = Create(PlayerMovement::Side::kServer);
  auto client = Create(PlayerMovement::Side::kClient);
  std::vector<MovementCall> on_server, on_client;
  for (int32_t number = 1; number <= 320; ++number) {
    const float yaw = std::fmod(number * 1.7f, 360.f);
    auto call = Command(number, 1, number % 40 < 20, yaw);
    call.left = number % 2 ? 1.f : -1.f;
    on_client.push_back(call);
    call.command = number - 1;
    on_server.push_back(call);
  }
  const auto a = Simulate(server, on_server, {0, 0, 0}, {});
  const auto b = Simulate(client, on_client, {0, 0, 0}, {});
  for (size_t i = 0; i < a.size(); ++i) {
    ASSERT_EQ(a[i].origin, b[i].origin) << "command " << i + 1;
    ASSERT_EQ(a[i].velocity, b[i].velocity) << "command " << i + 1;
  }
}

TEST(PlayerMovement, JumpsOnceUntilTheButtonIsReleased) {
  auto movement = Create(PlayerMovement::Side::kServer);
  std::vector<MovementCall> commands;
  for (int32_t number = 1; number <= 160; ++number) commands.push_back(Command(number, 0, true));
  const auto held = Simulate(movement, commands, {0, 0, 0}, {});
  // Quake adds 270 upward; gravity then removes one 16 ms step.
  EXPECT_NEAR(held[0].velocity[2], (270 - 800 * 0.015625f) * kScale, 0.01f);
  EXPECT_EQ(held[0].ground_entity, MovementHook::kNoGround);
  float apex = 0;
  int takeoffs = 0;
  for (size_t i = 1; i < held.size(); ++i) {
    apex = std::max(apex, held[i].origin[2]);
    takeoffs += held[i].velocity[2] > 300 && held[i - 1].velocity[2] <= 0;
  }
  EXPECT_NEAR(apex, 43.57f * kScale, 1);
  EXPECT_EQ(takeoffs, 0) << "holding jump through a landing must not jump again";
}

TEST(PlayerMovement, TheMantleKeyJumps) {
  auto movement = Create(PlayerMovement::Side::kClient);
  std::vector<MovementCall> commands;
  for (int32_t number = 1; number <= 120; ++number) {
    auto call = Command(number, 0, false);
    call.buttons[0] = PlayerMovement::kMantleButton;
    commands.push_back(call);
  }
  const auto held = Simulate(movement, commands, {0, 0, 0}, {});
  EXPECT_NEAR(held[0].velocity[2], (270 - 800 * 0.015625f) * kScale, 0.01f);
  EXPECT_EQ(held[0].ground_entity, MovementHook::kNoGround);
  int takeoffs = 0;
  for (size_t i = 1; i < held.size(); ++i)
    takeoffs += held[i].velocity[2] > 300 && held[i - 1].velocity[2] <= 0;
  EXPECT_EQ(takeoffs, 0) << "holding the key through the landing must not jump again";
}

TEST(PlayerMovement, LaunchedPlayersSlideDownTheWalls) {
  auto movement = Create(PlayerMovement::Side::kServer);
  std::vector<MovementCall> commands;
  for (int32_t number = 1; number <= 96; ++number) commands.push_back(Command(number, 0, false));
  const auto flight =
      Simulate(movement, commands, {0, 0, 900}, {900, 0, 0}, MovementHook::kNoGround);
  // Player collision stops at the wall minus the 16-unit half-width.
  constexpr float kWall = (kHalfWidth - 16) * kScale;
  for (const auto& step : flight) EXPECT_LE(step.origin[0], kWall + 0.05f);
  EXPECT_NEAR(flight.back().origin[0], kWall, 0.05f);
  EXPECT_NEAR(flight.back().velocity[0], 0, 0.01f) << "the wall absorbs the outward speed";
  EXPECT_LT(flight.back().origin[2], 900) << "the player keeps falling along the wall";
}

TEST(PlayerMovement, SubdividedCommandsMatchTheServer) {
  auto server = Create(PlayerMovement::Side::kServer);
  auto client = Create(PlayerMovement::Side::kClient);
  // Hold jump across commands, splitting each one at its midpoint as the
  // engine does when input changes within a tick.
  std::vector<MovementCall> commands;
  for (int32_t number = 1; number <= 48; ++number) {
    for (const auto& fractions : {std::array<float, 2>{0, 0.5f}, std::array<float, 2>{0.5f, 1}}) {
      auto call = Command(number, 1, true);
      call.fractions = fractions;
      commands.push_back(call);
    }
  }

  // The server's command field holds the previously processed number.
  auto on_server = commands;
  for (auto& call : on_server) --call.command;
  const auto authority = Simulate(server, on_server, {0, 0, 0}, {});
  const auto predicted = Simulate(client, commands, {0, 0, 0}, {});
  for (size_t i = 0; i < commands.size(); ++i) {
    EXPECT_EQ(predicted[i].origin, authority[i].origin) << "step " << i;
    EXPECT_EQ(predicted[i].velocity, authority[i].velocity) << "step " << i;
  }
}

TEST(PlayerMovement, PlayersStandOnHeadsAndReportLandings) {
  auto movement = Create(PlayerMovement::Side::kServer);
  int jumper = 0, below = 0;
  movement.SetPlayers({{.pawn = &jumper, .handle = 11, .feet = {0, 0, 200}},
                       {.pawn = &below, .handle = 22, .feet = {0, 0, 0}}});
  const auto fall = [&](float speed) {
    std::vector<MovementCall> commands;
    for (int32_t number = 1; number <= 24; ++number) {
      auto call = Command(number, 0, false);
      call.pawn = &jumper;
      commands.push_back(call);
    }
    return Simulate(movement, commands, {0, 0, 90}, {0, 0, -speed * kScale},
                    MovementHook::kNoGround)
        .back()
        .origin;
  };

  // The head is 56 Quake units up; resting there reports one landing on the
  // player below, at the speed the fall had.
  const auto rest = fall(300);
  EXPECT_NEAR(rest[2], 56 * kScale, 0.1f) << "players are solid to each other";
  const auto landings = movement.TakeLandings();
  ASSERT_EQ(landings.size(), 1u) << "standing on the head lands once";
  EXPECT_EQ(landings[0].player, 11u);
  EXPECT_EQ(landings[0].on, 22u);
  EXPECT_GT(landings[0].speed, 300 * kScale);

  // A client predicts the same landing without reporting it.
  auto client = Create(PlayerMovement::Side::kClient);
  client.SetPlayers({{.pawn = &jumper, .handle = 11, .feet = {0, 0, 200}},
                     {.pawn = &below, .handle = 22, .feet = {0, 0, 0}}});
  auto call = Command(1, 0, false);
  call.pawn = &jumper;
  call.origin = {0, 0, 71};
  call.velocity = {0, 0, -900};
  call.ground_entity = MovementHook::kNoGround;
  ASSERT_TRUE(client.Step(call));
  EXPECT_TRUE(client.TakeLandings().empty());
}

TEST(PlayerMovement, LeavesFrozenPawnsToNativeMovement) {
  auto movement = Create(PlayerMovement::Side::kServer);
  auto frozen = Command(1, 1, true);
  frozen.move_type = 0;
  frozen.origin = {-240, 0, 930};
  EXPECT_FALSE(movement.Step(frozen));
  EXPECT_EQ(frozen.origin, (std::array<float, 3>{-240, 0, 930}));
}

TEST(PlayerMovement, ClientReplayReproducesPredictedCommands) {
  auto predicted = Create(PlayerMovement::Side::kClient);
  std::vector<MovementCall> commands;
  for (int32_t number = 1; number <= 96; ++number) {
    const bool jump = number % 40 < 30;
    commands.push_back(Command(number, number % 3 ? 1.f : 0.5f, jump, number * 2.5f));
  }
  const auto first = Simulate(predicted, commands, {0, 0, 0}, {});

  // Replay from an acknowledged command, restoring its authoritative state.
  const size_t acknowledged = 51;
  const std::vector replayed_commands(commands.begin() + acknowledged + 1, commands.end());
  const auto replayed =
      Simulate(predicted, replayed_commands, first[acknowledged].origin,
               first[acknowledged].velocity, first[acknowledged].ground_entity);
  for (size_t i = 0; i < replayed.size(); ++i) {
    EXPECT_EQ(replayed[i].origin, first[acknowledged + 1 + i].origin) << "command " << i;
    EXPECT_EQ(replayed[i].velocity, first[acknowledged + 1 + i].velocity) << "command " << i;
  }
}

}  // namespace
}  // namespace modlock::quake
