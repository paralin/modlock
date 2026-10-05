// Contract tests for a unit restore: it replaces the units, finishes their
// placement a frame later, and succeeds only when the map holds each target as
// its own unit. A missing, extra or wrong unit and a malformed target each end
// it with a reason.
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "wasm/npc_restore.h"

namespace {

using modlock::wasm::NpcRestore;
using Target = NpcRestore::Target;
using Sample = NpcRestore::Sample;

Target Trooper(float x, uint32_t lane) {
  return {.designer_name = "npc_trooper",
          .subclass_id = 3,
          .team = 2,
          .position = {x, 0, 0},
          .facing = {},
          .velocity = {},
          .health = 200,
          .max_health = 200,
          .lane = lane};
}

Target Guardian() {
  return {.designer_name = "npc_trooper_boss",
          .subclass_id = 9,
          .team = 3,
          .position = {500, 500, 0},
          .facing = {},
          .velocity = {},
          .health = 4000,
          .max_health = 6000,
          .lane = std::nullopt};
}

// Map is a fake world. Restore places its targets exactly, unless drift
// moves them, and records the order of the writes.
struct Map {
  std::vector<Sample> units;
  std::vector<std::string> calls;
  float drift = 0;

  NpcRestore::Engine Engine() {
    return {
        .restore = [this](std::span<const Target> targets) -> std::expected<void, std::string> {
          calls.push_back("restore");
          units.clear();
          for (const auto& target : targets) {
            auto state = target;
            state.position[0] += drift;
            units.push_back({.handle = static_cast<uint32_t>(units.size() + 1), .state = state});
          }
          return {};
        },
        .finish = [this]() -> std::expected<void, std::string> {
          calls.push_back("finish");
          return {};
        },
        .read = [this] { return std::expected<std::vector<Sample>, std::string>(units); },
    };
  }

  NpcRestore::Result Run(NpcRestore& restore) {
    for (int frame = 0; frame < 3; ++frame) {
      if (auto result = restore.Tick()) return result;
    }
    return std::nullopt;
  }
};

TEST(NpcRestore, PlacedUnitsHold) {
  Map map;
  auto restore = NpcRestore::Create({Trooper(0, 1), Trooper(0, 4), Guardian()}, map.Engine());
  ASSERT_TRUE(restore) << restore.error();
  EXPECT_FALSE(restore->Tick());
  EXPECT_FALSE(restore->Tick());
  auto result = restore->Tick();
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result) << result->error();
  EXPECT_EQ(map.calls, (std::vector<std::string>{"restore", "finish"}));
}

TEST(NpcRestore, DriftBeyondReachFails) {
  Map map;
  map.drift = 40;
  auto restore = NpcRestore::Create({Trooper(0, 1)}, map.Engine());
  ASSERT_TRUE(restore) << restore.error();
  auto result = map.Run(*restore);
  ASSERT_TRUE(result);
  ASSERT_FALSE(*result);
  EXPECT_NE(result->error().find("the nearest is npc_trooper"), std::string::npos)
      << result->error();
}

TEST(NpcRestore, CountsNameTheKindThatDiffers) {
  std::vector<Target> targets{Trooper(0, 1), Guardian()};
  std::vector<Sample> samples{{.handle = 1, .state = Trooper(0, 1)},
                              {.handle = 2, .state = Trooper(0, 1)},
                              {.handle = 3, .state = Guardian()}};
  auto error = NpcRestore::Match(targets, samples);
  ASSERT_TRUE(error);
  EXPECT_NE(error->find("npc_trooper 3: 2 for 1"), std::string::npos) << *error;
  EXPECT_EQ(error->find("npc_trooper_boss"), std::string::npos) << *error;
}

TEST(NpcRestore, EachTargetNeedsItsOwnUnit) {
  // Two targets in the same spot must not both match the one trooper there.
  std::vector<Target> targets{Trooper(0, 1), Trooper(0, 1)};
  std::vector<Sample> samples{{.handle = 1, .state = Trooper(0, 1)},
                              {.handle = 2, .state = Trooper(0, 4)}};
  EXPECT_TRUE(NpcRestore::Match(targets, samples));

  samples[1].state.lane = 1;
  EXPECT_FALSE(NpcRestore::Match(targets, samples));
}

TEST(NpcRestore, RejectsMalformedTargets) {
  Map map;
  auto target = Trooper(0, 1);
  target.lane.reset();
  EXPECT_FALSE(NpcRestore::Create({target}, map.Engine()));

  target = Guardian();
  target.health = 0;
  EXPECT_FALSE(NpcRestore::Create({target}, map.Engine()));
  EXPECT_TRUE(map.calls.empty());
}

}  // namespace
