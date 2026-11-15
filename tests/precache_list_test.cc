#include <gtest/gtest.h>

#include "modlock/gameinterop/game_rules.h"

namespace modlock::gameinterop {
namespace {

TEST(PrecacheList, ANameWaitsForOneBuildThenStays) {
  // A new list has nothing for a build to register.
  PrecacheList list;
  EXPECT_FALSE(list.Pending());

  // New names wait for the next build, which registers them.
  EXPECT_TRUE(list.Add({"hero_inferno"}, {"particles/a.vpcf"}));
  EXPECT_TRUE(list.Pending());
  list.Built();
  EXPECT_FALSE(list.Pending());

  // Names the world already registered do not wait again.
  EXPECT_FALSE(list.Add({"hero_inferno"}, {"particles/a.vpcf"}));
  EXPECT_FALSE(list.Pending());

  // One new name among known ones waits, and only it is appended.
  EXPECT_TRUE(list.Add({"hero_inferno", "hero_haze"}, {}));
  EXPECT_TRUE(list.Pending());
  EXPECT_EQ(list.heroes(), (std::vector<std::string>{"hero_inferno", "hero_haze"}));
  EXPECT_EQ(list.resources(), (std::vector<std::string>{"particles/a.vpcf"}));

  // A repeat before the build keeps the name waiting.
  EXPECT_FALSE(list.Add({"hero_haze"}, {}));
  EXPECT_TRUE(list.Pending());
}

TEST(PrecacheList, ADuplicateWithinOneCallIsAddedOnce) {
  PrecacheList list;
  EXPECT_TRUE(list.Add({}, {"models/b.vmdl", "models/b.vmdl"}));
  EXPECT_EQ(list.resources(), (std::vector<std::string>{"models/b.vmdl"}));
}

}  // namespace
}  // namespace modlock::gameinterop
