// Contract tests for a hero restore: it writes the build, settles, then health,
// pose and timers rebased to the live clock, and succeeds only once the hero
// held the target for a second. A death, a write that never takes and a
// malformed target each end it with a reason.
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "wasm/hero_restore.h"

namespace {

using modlock::wasm::HeroRestore;
using modlock::wasm::RestoreTarget;
using Ability = HeroRestore::Ability;

constexpr uint32_t kAbility = 100;
constexpr uint32_t kItem = 200;

// World is a fake game holding one hero. Apply writes into it the way the
// engine would, and Frame advances its clock by one server frame.
struct World {
  HeroRestore::Sample sample;
  std::vector<Ability> owned;
  float time = 100;
  int32_t tick = 6400;
  // health_sticks is false to model a game that undoes the health write.
  bool health_sticks = true;
  // grow adds an upgrade tier to the ability after the build applies, as a
  // bot spending points would.
  bool grow = false;
  // fall lowers the hero each frame, as gravity does a hero placed in the
  // air, and push offsets each pose write, as collision does.
  float fall = 0;
  float push = 0;
  int builds = 0;

  World() {
    // Start a level hero with full stamina and two upgrades.
    sample.slot = 2;
    sample.steam_id = 7;
    sample.pawn_handle = 42;
    sample.health = 500;
    sample.max_health = 600;
    sample.effective_max_health = 650;
    sample.stamina = HeroRestore::Sample::Stamina{.current = 1, .max = 3};
    owned = {{.handle = 1, .subclass_id = kAbility, .slot = 0, .upgrade_info = 0x10005},
             {.handle = 2, .subclass_id = kItem, .slot = 4, .upgrade_info = 0x10000}};
  }

  HeroRestore::Engine Engine() {
    return {
        .apply = [this](const HeroRestore::Sample&, const RestoreTarget& target,
                        HeroRestore::Stage stage) { return Apply(target, stage); },
        .abilities =
            [this](int32_t) { return std::expected<std::vector<Ability>, std::string>(owned); },
        .definition = [](uint32_t id)
            -> std::expected<modlock::gameinterop::AbilityDefinitions::Definition, std::string> {
          return modlock::gameinterop::AbilityDefinitions::Definition{
              .name = id == kItem ? "upgrade_sprint_booster" : "citadel_ability_dash",
              .disabled = false,
              .native_definition_pointer = nullptr};
        },
        .clock =
            [this] {
              return std::expected<modlock::gameinterop::EngineServer::SimulationClock,
                                   std::string>(
                  {.current_time = time, .tick = tick, .interval = 1.0f / 64});
            },
        .set_timers = [this](int32_t,
                             std::span<const Ability> timers) -> std::expected<void, std::string> {
          for (const auto& timer : timers) {
            for (auto& ability : owned) {
              if (ability.handle == timer.handle) {
                ability = timer;
              }
            }
          }
          return {};
        },
        .ready_timers =
            [this](int32_t) {
              auto ready = owned;
              for (auto& ability : ready) {
                ability.charges = 2;
                ability.cooldown_start = ability.cooldown_end = 0;
                ability.charge_recharge_start = ability.charge_recharge_end = 0;
              }
              return std::expected<std::vector<Ability>, std::string>(ready);
            },
    };
  }

  bool Apply(const RestoreTarget& target, HeroRestore::Stage stage) {
    // The build stage sets the level, upgrades and bonuses.
    if (stage == HeroRestore::Stage::kBuild) {
      ++builds;
      if (target.level) {
        sample.level = *target.level;
      }
      for (const auto& upgrade : target.abilities) {
        for (auto& ability : owned) {
          if (ability.subclass_id == upgrade.subclass_id) {
            ability.upgrade_info = upgrade.upgrade_info | (grow ? 0x80000u : 0u);
          }
        }
      }
      if (target.upgrade_bonuses) {
        sample.upgrade_bonuses = target.upgrade_bonuses;
      }
      return true;
    }

    // The pawn stage fills a fresh hero, or sets the saved health.
    if (stage == HeroRestore::Stage::kPawn) {
      if (target.fresh) {
        sample.stamina->current = sample.stamina->max;
        if (health_sticks) {
          sample.health = *sample.effective_max_health;
        }
      } else {
        if (health_sticks) {
          sample.health = target.health;
        }
        sample.max_health = target.max_health;
      }
    }

    // Every stage places the hero, pushed aside by the world.
    sample.x = target.position[0] + push;
    sample.y = target.position[1] + push;
    sample.z = target.position[2] + push;
    sample.camera_angles = target.angles;
    return true;
  }

  // Run ticks restore until it ends or frames run out.
  HeroRestore::Result Run(HeroRestore& restore, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
      if (auto result = restore.Tick(sample, static_cast<uint64_t>(tick))) {
        return result;
      }
      time += 1.0f / 64;
      ++tick;
      sample.z -= fall;
    }
    return std::nullopt;
  }
};

RestoreTarget Exact() {
  return {
      .position = {10, 20, 30},
      .angles = {5, 90, 0},
      .level = 9,
      .health = 320,
      .max_health = 700,
      .upgrade_bonuses = std::array<float, 3>{1, 2, 3},
      .abilities = {{.subclass_id = kAbility, .slot = 0, .upgrade_info = 0x30005}},
      .timers = {{.subclass_id = kAbility, .charges = 1, .cooldown = {-2, 6}}},
  };
}

TEST(HeroRestore, ExactTargetHolds) {
  // Restore the exact target.
  World world;
  auto restore = HeroRestore::Create(world.sample, Exact(), world.Engine());
  ASSERT_TRUE(restore) << restore.error();
  auto result = world.Run(*restore, 30 * 64);
  ASSERT_TRUE(result);
  ASSERT_TRUE(*result) << result->error();

  // Health, level and upgrades hold.
  EXPECT_EQ(world.sample.health, 320);
  EXPECT_EQ(world.sample.level, 9);
  EXPECT_EQ(world.owned[0].upgrade_info, 0x30005u);

  // The cooldown was rebased to the clock when the timers were written, a
  // second after the build.
  EXPECT_EQ(world.owned[0].charges, 1);
  EXPECT_FLOAT_EQ(world.owned[0].cooldown_end - world.owned[0].cooldown_start, 8);
  EXPECT_NEAR(world.owned[0].cooldown_start, 100 + 1 - 2, 0.1);
}

TEST(HeroRestore, FreshTargetFillsHealthAndStamina) {
  // Restore a fresh hero.
  World world;
  RestoreTarget target{.position = {1, 2, 3}, .fresh = true};
  auto restore = HeroRestore::Create(world.sample, target, world.Engine());
  ASSERT_TRUE(restore) << restore.error();
  auto result = world.Run(*restore, 30 * 64);
  ASSERT_TRUE(result && *result) << (result ? result->error() : "still running");

  // Health and stamina fill, and the charges refill.
  EXPECT_EQ(world.sample.health, 650);
  EXPECT_EQ(world.sample.stamina->current, 3);
  EXPECT_EQ(world.owned[0].charges, 2);
}

TEST(HeroRestore, FreshTargetLetsTheHeroFallOrBePushed) {
  // A fresh hero that falls or is pushed still restores.
  World world;
  world.fall = 8;
  world.push = 14;
  RestoreTarget target{.position = {1, 2, 900}, .fresh = true};
  auto restore = HeroRestore::Create(world.sample, target, world.Engine());
  ASSERT_TRUE(restore) << restore.error();
  auto result = world.Run(*restore, 30 * 64);
  ASSERT_TRUE(result && *result) << (result ? result->error() : "still running");
}

TEST(HeroRestore, FreshBotKeepsBoughtUpgrades) {
  // Make a bot whose upgrades grow past the target.
  World world;
  world.sample.is_bot = true;
  world.grow = true;
  RestoreTarget target{
      .position = {1, 2, 3},
      .fresh = true,
      .abilities = {{.subclass_id = kAbility, .slot = 0, .upgrade_info = 0x30005}}};

  // It still restores, keeping the grown upgrades.
  auto restore = HeroRestore::Create(world.sample, target, world.Engine());
  ASSERT_TRUE(restore) << restore.error();
  auto result = world.Run(*restore, 30 * 64);
  ASSERT_TRUE(result && *result) << (result ? result->error() : "still running");
  EXPECT_EQ(world.owned[0].upgrade_info, 0xb0005u);
}

TEST(HeroRestore, PersonsUpgradesMustMatch) {
  // A person's upgrades that grow past the target fail.
  World world;
  world.grow = true;
  auto restore = HeroRestore::Create(world.sample, Exact(), world.Engine());
  ASSERT_TRUE(restore) << restore.error();
  auto result = world.Run(*restore, 30 * 64);
  ASSERT_TRUE(result);
  ASSERT_FALSE(*result);
  EXPECT_NE(result->error().find("abilities differ"), std::string::npos) << result->error();
}

TEST(HeroRestore, HealthThatNeverTakesFails) {
  // Health that never takes fails and names the health.
  World world;
  world.health_sticks = false;
  auto restore = HeroRestore::Create(world.sample, Exact(), world.Engine());
  ASSERT_TRUE(restore) << restore.error();
  auto result = world.Run(*restore, 30 * 64);
  ASSERT_TRUE(result);
  ASSERT_FALSE(*result);
  EXPECT_NE(result->error().find("health 500 of 320"), std::string::npos) << result->error();
}

TEST(HeroRestore, DeathEndsTheRestore) {
  // Start a restore.
  World world;
  auto restore = HeroRestore::Create(world.sample, Exact(), world.Engine());
  ASSERT_TRUE(restore) << restore.error();
  EXPECT_FALSE(world.Run(*restore, 10));

  // A new pawn means the hero died; the restore ends.
  world.sample.pawn_handle = 43;
  auto result = world.Run(*restore, 1);
  ASSERT_TRUE(result);
  EXPECT_FALSE(*result);
}

TEST(HeroRestore, RejectsMalformedTargets) {
  // A zero health target is refused.
  World world;
  auto target = Exact();
  target.health = 0;
  EXPECT_FALSE(HeroRestore::Create(world.sample, target, world.Engine()));

  // A nonfinite position is refused.
  target = Exact();
  target.position[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(HeroRestore::Create(world.sample, target, world.Engine()));

  // An item in a slot past the shop is refused.
  target = Exact();
  target.items = std::vector<modlock::gameinterop::ItemTarget>{
      {.subclass_id = kItem, .upgrade_info = 0x10000, .slot = 9}};
  EXPECT_FALSE(HeroRestore::Create(world.sample, target, world.Engine()));

  // A dead hero is refused.
  world.sample.health = 0;
  EXPECT_FALSE(HeroRestore::Create(world.sample, Exact(), world.Engine()));
}

}  // namespace
