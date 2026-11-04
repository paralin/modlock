// A host roster applies to a connection once, again on reconnect or seat
// change, and never confirms a failed or superseded assignment.
#include "wasm/team_seating.h"

#include "gtest/gtest.h"

TEST(TeamSeating, JoinsReconnectsAndReplacementsKeepOnlyCurrentConfirmations) {
  // The host assigns two playing sides and an observer before they connect.
  modlock::wasm::TeamSeating seating;
  modlock::wasm::TeamAssignments roster;
  auto& teams = *roster.mutable_assignments();
  teams[11] = 2;
  teams[22] = 3;
  teams[33] = 1;
  ASSERT_TRUE(seating.Assign(roster));

  // Both sides and the observer get their own targets; outsiders keep choice.
  EXPECT_EQ(seating.Pending(11, 0, 1), 2);
  EXPECT_EQ(seating.Pending(22, 1, 1), 3);
  EXPECT_EQ(seating.Pending(33, 2, 1), 1);
  EXPECT_FALSE(seating.Pending(44, 3, 1));

  // A failed attempt stays pending; a mismatched native team cannot confirm.
  seating.Confirm(11, 0, 1, 3);
  EXPECT_EQ(seating.Pending(11, 0, 1), 2);
  seating.Confirm(11, 0, 1, 2);
  seating.Confirm(33, 2, 1, 1);
  EXPECT_FALSE(seating.Pending(11, 0, 1));
  EXPECT_FALSE(seating.Pending(33, 2, 1));

  // Reapplying the same roster, as after reload, leaves confirmed players
  // alone. Reconnecting the same identity applies its seat again.
  ASSERT_TRUE(seating.Assign(roster));
  EXPECT_FALSE(seating.Pending(11, 0, 1));
  EXPECT_EQ(seating.Pending(11, 0, 2), 2);
  seating.Confirm(11, 0, 2, 2);

  // A new slot can have the same generation as the previous slot.
  EXPECT_EQ(seating.Pending(11, 5, 2), 2);
  seating.Confirm(11, 5, 2, 2);
  EXPECT_FALSE(seating.Pending(11, 5, 2));

  // Moving one player changes only their target; the observer stays seated.
  teams[11] = 3;
  ASSERT_TRUE(seating.Assign(roster));
  EXPECT_EQ(seating.Pending(11, 0, 2), 3);
  EXPECT_FALSE(seating.Pending(33, 2, 1));
  seating.Confirm(11, 0, 2, 2);
  EXPECT_EQ(seating.Pending(11, 0, 2), 3);

  // Invalid input cannot replace the valid roster.
  teams[22] = 4;
  EXPECT_FALSE(seating.Assign(roster));
  EXPECT_EQ(seating.Pending(22, 1, 1), 3);

  // Removing a seat and adding it again requires fresh confirmation.
  teams[22] = 3;
  teams.erase(11);
  ASSERT_TRUE(seating.Assign(roster));
  EXPECT_FALSE(seating.Pending(11, 0, 2));
  teams[11] = 2;
  ASSERT_TRUE(seating.Assign(roster));
  EXPECT_EQ(seating.Pending(11, 0, 2), 2);

  // A map change replaces controllers even when connections stay present.
  seating.Confirm(11, 0, 2, 2);
  seating.WorldEnding();
  EXPECT_EQ(seating.Pending(11, 0, 2), 2);
  EXPECT_EQ(seating.Pending(33, 2, 1), 1);

  // Releasing the roster restores the mod's control of every player.
  roster.clear_assignments();
  ASSERT_TRUE(seating.Assign(roster));
  EXPECT_TRUE(seating.Empty());
}
