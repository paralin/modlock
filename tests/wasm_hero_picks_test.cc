// Contract tests for HeroPicks: a player's own Change Hero pick holds off a
// mod's seat until it lands, runs out of time or the player reconnects.
#include "gtest/gtest.h"
#include "wasm/hero_picks.h"

namespace {

using modlock::wasm::HeroPicks;

constexpr int32_t kSlot = 1;
constexpr uint32_t kGeneration = 4;
constexpr uint32_t kOld = 6;
constexpr uint32_t kPick = 9;

TEST(HeroPicks, NoPickLeavesTheSeatToTheMod) {
  HeroPicks picks;
  EXPECT_FALSE(picks.Arriving(kSlot, kGeneration, 0, 10));
}

TEST(HeroPicks, ThePickHoldsThroughTheGapUntilItLands) {
  // The old hero stays a moment, then the game removes it before the pick.
  HeroPicks picks;
  picks.Picked(kSlot, kGeneration, kPick, 10);
  EXPECT_EQ(picks.Arriving(kSlot, kGeneration, kOld, 10.1), kPick);
  EXPECT_EQ(picks.Arriving(kSlot, kGeneration, 0, 11), kPick);

  // The pick lands and is forgotten, so a later loss is the mod's to seat.
  EXPECT_FALSE(picks.Arriving(kSlot, kGeneration, kPick, 12));
  EXPECT_FALSE(picks.Arriving(kSlot, kGeneration, 0, 13));
}

TEST(HeroPicks, APickThatNeverLandsFreesTheSeat) {
  HeroPicks picks;
  picks.Picked(kSlot, kGeneration, kPick, 10);
  EXPECT_EQ(picks.Arriving(kSlot, kGeneration, 0, 10 + HeroPicks::kSeconds - 0.01), kPick);
  EXPECT_FALSE(picks.Arriving(kSlot, kGeneration, 0, 10 + HeroPicks::kSeconds));
}

TEST(HeroPicks, AReconnectOrLeaveDropsThePick) {
  HeroPicks picks;
  picks.Picked(kSlot, kGeneration, kPick, 10);
  EXPECT_FALSE(picks.Arriving(kSlot, kGeneration + 1, 0, 11));
  picks.Picked(kSlot, kGeneration, kPick, 12);
  picks.Forget(kSlot);
  EXPECT_FALSE(picks.Arriving(kSlot, kGeneration, 0, 13));
}

}  // namespace
