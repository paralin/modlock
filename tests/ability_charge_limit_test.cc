// Contract tests for the ability charge limit: a limited ability answers its
// limit through the dispatch table and every other ability keeps the native
// answer.
#include "modlock/gameinterop/ability_charge_limit.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using modlock::gameinterop::AbilityChargeLimit;

constexpr size_t kSlot = 9;

// FakeAbility is an object that dispatches through a table, as the game's
// abilities do: the first word points at the table.
struct FakeAbility {
  explicit FakeAbility(void** table, uint32_t handle) : table(table), handle(handle) {}
  void** table;
  uint32_t handle;
};

int32_t NativeThree(void*) {
  return 3;
}
int32_t NativeTwo(void*) {
  return 2;
}

// leaving makes every ability look as if it is being deleted.
bool leaving = false;

std::optional<uint32_t> HandleOf(void* ability) {
  if (leaving) {
    return std::nullopt;
  }
  return static_cast<FakeAbility*>(ability)->handle;
}

// Query dispatches GetMaxCharges the way the game's callers do.
int32_t Query(FakeAbility& ability) {
  return reinterpret_cast<int32_t (*)(void*)>(ability.table[kSlot])(&ability);
}

struct Tables {
  Tables() {
    three.fill(nullptr);
    two.fill(nullptr);
    three[kSlot] = reinterpret_cast<void*>(&NativeThree);
    two[kSlot] = reinterpret_cast<void*>(&NativeTwo);
  }
  std::array<void*, 16> three;
  std::array<void*, 16> two;
};

TEST(AbilityChargeLimit, AnswersTheLimitOfALimitedAbility) {
  // Limit an ability of a three charge class to five.
  Tables tables;
  FakeAbility ability(tables.three.data(), 11);
  auto limit = AbilityChargeLimit::Create(kSlot, HandleOf);
  ASSERT_TRUE(limit);
  ASSERT_TRUE((*limit)->Set(&ability, 11, 5));

  // The ability and the table both answer five.
  EXPECT_EQ(Query(ability), 5);
  EXPECT_EQ((*limit)->Limit(11), 5);
}

TEST(AbilityChargeLimit, KeepsTheNativeAnswerForOtherAbilitiesOfTheClass) {
  // Limit one ability of a class.
  Tables tables;
  FakeAbility limited(tables.three.data(), 11);
  FakeAbility other(tables.three.data(), 12);
  auto limit = AbilityChargeLimit::Create(kSlot, HandleOf);
  ASSERT_TRUE(limit);
  ASSERT_TRUE((*limit)->Set(&limited, 11, 5));

  // Another ability of the class keeps the native answer.
  EXPECT_EQ(Query(other), 3);
}

TEST(AbilityChargeLimit, KeepsEachClassNativeAnswer) {
  // Limit an ability of each class.
  Tables tables;
  FakeAbility three(tables.three.data(), 11);
  FakeAbility two(tables.two.data(), 12);
  auto limit = AbilityChargeLimit::Create(kSlot, HandleOf);
  ASSERT_TRUE(limit);
  ASSERT_TRUE((*limit)->Set(&three, 11, 4));
  ASSERT_TRUE((*limit)->Set(&two, 12, 1));

  // Each answers its own limit.
  EXPECT_EQ(Query(three), 4);
  EXPECT_EQ(Query(two), 1);
}

TEST(AbilityChargeLimit, ZeroReturnsTheAbilityToTheGame) {
  // Limit an ability, then set it to zero.
  Tables tables;
  FakeAbility ability(tables.three.data(), 11);
  auto limit = AbilityChargeLimit::Create(kSlot, HandleOf);
  ASSERT_TRUE(limit);
  ASSERT_TRUE((*limit)->Set(&ability, 11, 5));
  ASSERT_TRUE((*limit)->Set(&ability, 11, 0));

  // The game answers again.
  EXPECT_EQ(Query(ability), 3);
  EXPECT_EQ((*limit)->Limit(11), std::nullopt);
}

TEST(AbilityChargeLimit, LimitsNoAbilityCreatedAtTheSameAddress) {
  // Limit an ability.
  Tables tables;
  FakeAbility ability(tables.three.data(), 11);
  auto limit = AbilityChargeLimit::Create(kSlot, HandleOf);
  ASSERT_TRUE(limit);
  ASSERT_TRUE((*limit)->Set(&ability, 11, 5));

  // A new ability at the same address has a new serial in its handle.
  ability.handle = 11 | (1u << 15);
  EXPECT_EQ(Query(ability), 3);
}

TEST(AbilityChargeLimit, RejectsWhatItCannotLimit) {
  // Create a limit.
  Tables tables;
  FakeAbility ability(tables.three.data(), 11);
  auto limit = AbilityChargeLimit::Create(kSlot, HandleOf);
  ASSERT_TRUE(limit);

  // A null ability, a wrong handle or an out of range count is refused.
  EXPECT_FALSE((*limit)->Set(nullptr, 11, 5));
  EXPECT_FALSE((*limit)->Set(&ability, 12, 5));
  EXPECT_FALSE((*limit)->Set(&ability, 11, -1));
  EXPECT_FALSE((*limit)->Set(&ability, 11, AbilityChargeLimit::kMostCharges + 1));
  EXPECT_EQ(Query(ability), 3);
}

TEST(AbilityChargeLimit, AnswersTheNativeMaximumOfALeavingAbility) {
  // Limit an ability.
  Tables tables;
  FakeAbility ability(tables.three.data(), 11);
  auto limit = AbilityChargeLimit::Create(kSlot, HandleOf);
  ASSERT_TRUE(limit);
  ASSERT_TRUE((*limit)->Set(&ability, 11, 5));

  // While the ability leaves, the native maximum answers.
  leaving = true;
  EXPECT_EQ(Query(ability), 3);
  leaving = false;
}

TEST(AbilityChargeLimit, AllowsOneAtATime) {
  auto first = AbilityChargeLimit::Create(kSlot, HandleOf);
  ASSERT_TRUE(first);
  EXPECT_FALSE(AbilityChargeLimit::Create(kSlot, HandleOf));
}

TEST(AbilityChargeLimit, RestoresTheTableWhenDestroyed) {
  // Limit an ability for one scope.
  Tables tables;
  FakeAbility ability(tables.three.data(), 11);
  {
    auto limit = AbilityChargeLimit::Create(kSlot, HandleOf);
    ASSERT_TRUE(limit);
    ASSERT_TRUE((*limit)->Set(&ability, 11, 5));
  }

  // The table and the answer are back.
  EXPECT_EQ(tables.three[kSlot], reinterpret_cast<void*>(&NativeThree));
  EXPECT_EQ(Query(ability), 3);
}

}  // namespace
