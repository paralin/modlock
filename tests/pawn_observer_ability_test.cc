// Contract tests for the pawn observer's abilities, items, stamina and
// timers, over the same recorded entity images.
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <string>

#include "gtest/gtest.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "pawn_observer_fixture.h"

namespace pawn_observer_test {

namespace {

TEST(PawnObserver, ReadsOwnedAbilitiesAndRejectsIncompleteOrStaleLoadouts) {
  // Seat a player with a controller and a pawn.
  Fixture fixture;
  fixture.occupied = true;
  const uint32_t pawn_handle = HandleOf(2, 7);
  fixture.controller = MakeController(pawn_handle);
  Instance pawn{}, body{}, scene{}, ability{};
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  WritePointer(body.data() + kSceneNodeField, scene.data());

  // Register the controller, pawn and ability.
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);
  fixture.image.SetIdentity(3, ability.data(), 9, 0);

  // Give the pawn a handle vector whose live entry is the ability, which it owns.
  const uint32_t ability_handle = HandleOf(3, 9);
  uint32_t handles[] = {0xFFFFFFFF, ability_handle, ability_handle};
  const int32_t count = 2;
  const uint32_t capacity = 3;
  std::memcpy(pawn.data() + 0xc0, &count, sizeof(count));
  WritePointer(pawn.data() + 0xc8, handles);
  std::memcpy(pawn.data() + 0xd0, &capacity, sizeof(capacity));
  std::memcpy(ability.data() + 0x30, &pawn_handle, sizeof(pawn_handle));

  // Give the ability its subclass, slot and wall jump count.
  const uint32_t subclass = 0xf1234567;
  const uint16_t slot = 12;
  ability[0x3A] = 2;
  std::memcpy(ability.data() + 0x34, &subclass, sizeof(subclass));
  std::memcpy(ability.data() + 0x38, &slot, sizeof(slot));

  // Choose its upgrades, charges and timers.
  const uint32_t upgrade_info = 0x70001;
  const int32_t charges = 2;
  const float cooldown = 123.25f;
  const float cooldown_start = 100.5f, recharge_start = 105.75f, recharge_end = 110.25f;

  // Write them into the ability.
  std::memcpy(ability.data() + 0x48, &cooldown_start, sizeof(cooldown_start));
  std::memcpy(ability.data() + 0x4c, &recharge_start, sizeof(recharge_start));
  std::memcpy(ability.data() + 0x50, &recharge_end, sizeof(recharge_end));
  std::memcpy(ability.data() + 0x3c, &upgrade_info, sizeof(upgrade_info));
  std::memcpy(ability.data() + 0x40, &charges, sizeof(charges));
  std::memcpy(ability.data() + 0x44, &cooldown, sizeof(cooldown));

  // Observe through the ability layout and the movement layout.
  auto seams = SeamsFor(fixture);
  seams.ability_layout = []() -> std::expected<modlock::gameinterop::AbilityLayout, std::string> {
    return modlock::gameinterop::AbilityLayout{.handles = 0xc0,
                                               .owner = 0x30,
                                               .subclass = 0x34,
                                               .slot = 0x38,
                                               .consecutive_wall_jumps = 0x3A,
                                               .upgrade_info = 0x3c,
                                               .charges = 0x40,
                                               .cooldown_end = 0x44,
                                               .cooldown_start = 0x48,
                                               .charge_recharge_start = 0x4c,
                                               .charge_recharge_end = 0x50};
  };
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    return MovementFixtureLayout();
  };
  PawnObserver observer{std::move(seams), true};

  // Abilities read only after an observation, and movement reads the jump ability.
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  const auto observed = observer.Observe();
  ASSERT_TRUE(observed);
  ASSERT_TRUE(observed->movement);
  EXPECT_EQ(observed->movement->jump_ability_handle, ability_handle);
  EXPECT_EQ(observed->movement->consecutive_wall_jumps, 2);

  // The loadout holds the one live ability.
  auto actual = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(actual) << actual.error();
  ASSERT_EQ(actual->size(), 1u);
  EXPECT_EQ(actual->front().handle, ability_handle);
  EXPECT_EQ(actual->front().subclass_id, subclass);
  EXPECT_EQ(actual->front().slot, slot);
  EXPECT_EQ(actual->front().consecutive_wall_jumps, 2);

  // An invalid wall jump count reads as absent.
  ability[0x3A] = 0xFF;
  const auto invalid_count = observer.Observe();
  ASSERT_TRUE(invalid_count);
  EXPECT_FALSE(invalid_count->movement->consecutive_wall_jumps);
  ability[0x3A] = 0;

  // The ability carries its upgrades and timers, and only for its own slot.
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(1));
  EXPECT_EQ(actual->front().upgrade_info, upgrade_info);
  EXPECT_EQ(actual->front().charges, charges);
  EXPECT_EQ(actual->front().cooldown_end, cooldown);
  EXPECT_EQ(actual->front().cooldown_start, cooldown_start);
  EXPECT_EQ(actual->front().charge_recharge_start, recharge_start);
  EXPECT_EQ(actual->front().charge_recharge_end, recharge_end);

  // A timer that is not finite rejects the loadout.
  for (const auto offset : {0x48, 0x4c, 0x50}) {
    // An infinite timer field fails the read; restore it after.
    float saved;
    std::memcpy(&saved, ability.data() + offset, sizeof(saved));
    const float invalid = std::numeric_limits<float>::infinity();
    std::memcpy(ability.data() + offset, &invalid, sizeof(invalid));
    EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
    std::memcpy(ability.data() + offset, &saved, sizeof(saved));
  }
  const float invalid_cooldown = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(ability.data() + 0x44, &invalid_cooldown, sizeof(invalid_cooldown));
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  std::memcpy(ability.data() + 0x44, &cooldown, sizeof(cooldown));
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(1));

  // Count the upgrade bits writes.
  static int writes = 0;
  writes = 0;
  const auto set_bits = +[](void* entity, uint32_t bits) {
    ++writes;
    std::memcpy(static_cast<unsigned char*>(entity) + 0x3e, &bits, sizeof(uint16_t));
  };

  // An invalid batch writes nothing.
  const modlock::gameinterop::AbilityUpgrade target{subclass, slot, 0xf0001};
  const std::array invalid_targets{target, modlock::gameinterop::AbilityUpgrade{123, 2, 0x10001}};
  EXPECT_FALSE(observer.ApplyAbilityUpgrades(0, invalid_targets, set_bits));
  EXPECT_EQ(writes, 0);  // Validate the whole request before touching the first ability.

  // A valid one writes once, then never again.
  const std::array targets{target};
  ASSERT_TRUE(observer.ApplyAbilityUpgrades(0, targets, set_bits));
  EXPECT_EQ(writes, 1);
  ASSERT_TRUE(observer.ApplyAbilityUpgrades(0, targets, set_bits));
  EXPECT_EQ(writes, 1);  // An already restored mask has no second engine side effect.

  // A different low word cannot be restored.
  const std::array wrong_low_word{modlock::gameinterop::AbilityUpgrade{subclass, slot, 0xf0002}};
  EXPECT_FALSE(observer.ApplyAbilityUpgrades(0, wrong_low_word, set_bits));
  EXPECT_EQ(writes, 1);

  // The final engine setter can replace the selected pawn. A caller must not
  // receive success and continue restoring bonuses on the old borrowed pawn.
  static unsigned char* changing_controller = nullptr;
  changing_controller = fixture.controller.data();
  std::memcpy(ability.data() + 0x3c, &upgrade_info, sizeof(upgrade_info));
  const auto replace_pawn = +[](void*, uint32_t) {
    const uint32_t replacement = HandleOf(2, 8);
    std::memcpy(changing_controller + kPawnHandleField, &replacement, sizeof(replacement));
  };
  auto changed = observer.ApplyAbilityUpgrades(0, targets, replace_pawn);
  EXPECT_FALSE(changed);
  if (!changed) {
    EXPECT_EQ(changed.error(), "player pawn changed during the call");
  }

  // Restore the pawn.
  std::memcpy(fixture.controller.data() + kPawnHandleField, &pawn_handle, sizeof(pawn_handle));
  changing_controller = nullptr;

  // Every call locates the slot's current pawn, so a reconnect needs no new
  // observation first.
  ++fixture.generation;
  ASSERT_TRUE(observer.CurrentAbilitiesForSlot(0));

  // A reused identity cannot satisfy the older handle in the pawn's vector.
  fixture.image.SetIdentity(3, ability.data(), 10, 0);
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  fixture.image.SetIdentity(3, nullptr, 9, 0);
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  fixture.image.SetIdentity(3, ability.data(), 9, 0);

  // An ability owned by another pawn rejects the loadout.
  const uint32_t wrong_owner = HandleOf(2, 6);
  std::memcpy(ability.data() + 0x30, &wrong_owner, sizeof(wrong_owner));
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  std::memcpy(ability.data() + 0x30, &pawn_handle, sizeof(pawn_handle));

  // So does a vector that names the ability twice.
  const int32_t duplicate_count = 3;
  std::memcpy(pawn.data() + 0xc0, &duplicate_count, sizeof(duplicate_count));
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));

  // Two jump abilities leave the movement's jump ability ambiguous.
  Instance second_ability = ability;
  fixture.image.SetIdentity(4, second_ability.data(), 10, 0);
  handles[2] = HandleOf(4, 10);
  const auto ambiguous = observer.Observe();
  ASSERT_TRUE(ambiguous);
  ASSERT_TRUE(ambiguous->movement);
  EXPECT_FALSE(ambiguous->movement->jump_ability_handle);
  EXPECT_FALSE(ambiguous->movement->consecutive_wall_jumps);

  // An oversized vector is rejected; an empty one reads as no abilities.
  const int32_t oversized_count = 257;
  std::memcpy(pawn.data() + 0xc0, &oversized_count, sizeof(oversized_count));
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  const int32_t empty_count = 0;
  std::memcpy(pawn.data() + 0xc0, &empty_count, sizeof(empty_count));
  actual = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(actual);
  EXPECT_TRUE(actual->empty());

  // An empty slot has no frame.
  fixture.occupied = false;
  EXPECT_FALSE(observer.Observe());
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
}

TEST(PawnObserver, GrantsAnItemOnceAndPreservesTheExistingLoadout) {
  // Observe a pawn that has room for a granted item.
  ItemFixture fixture;
  fixture.connection.image.SetIdentity(5, fixture.granted_item.data(), 11, 0);
  PawnObserver observer(fixture.Seams());
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  auto functions = ItemFixture::Functions();

  // The engine grants the item into the third handle.
  functions.add = +[](void* pawn, const char* name, uint64_t upgrade, void* extra) -> void* {
    // Check the grant's arguments.
    auto& fixture = *ItemFixture::current;
    EXPECT_EQ(pawn, fixture.pawn.data());
    EXPECT_STREQ(name, "upgrade_new");
    EXPECT_EQ(upgrade, 0u);
    EXPECT_EQ(extra, nullptr);

    // Count it and append the granted item.
    ++fixture.adds;
    fixture.InitAbility(fixture.granted_item, 201, 4);
    fixture.handles[2] = HandleOf(5, 11);
    fixture.SetCount(3);
    return fixture.granted_item.data();
  };

  // Unknown and non-item subclasses grant nothing.
  EXPECT_FALSE(observer.GrantItem(0, 999, definitions, functions));
  EXPECT_FALSE(observer.GrantItem(0, 100, definitions, functions));
  EXPECT_EQ(fixture.adds, 0);

  // A valid item grants once, into its slot.
  const auto item = observer.GrantItem(0, 201, definitions, functions);
  ASSERT_TRUE(item) << item.error();
  EXPECT_EQ(item->slot, 4);
  EXPECT_EQ(item->handle, HandleOf(5, 11));
  ASSERT_TRUE(observer.GrantItem(0, 201, definitions, functions));
  EXPECT_EQ(fixture.adds, 1);
  EXPECT_EQ(fixture.removes, 0);

  // The existing loadout stays in front.
  const auto owned = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(owned);
  ASSERT_EQ(owned->size(), 3);
  EXPECT_EQ(owned->at(0).subclass_id, 100);
  EXPECT_EQ(owned->at(1).subclass_id, 200);
}

TEST(PawnObserver, ReconcilesItemsIdempotentlyWithoutRemovingHeroAbilities) {
  // Observe the fixture's loadout.
  ItemFixture fixture;
  PawnObserver observer(fixture.Seams());
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  const std::array targets{modlock::gameinterop::ItemTarget{201, 0x10001}};
  const auto functions = ItemFixture::Functions();

  // A batch with an unknown item changes nothing.
  const std::array invalid{targets[0], modlock::gameinterop::ItemTarget{999, 0x10001}};
  EXPECT_FALSE(observer.ReconcileItems(0, invalid, definitions, functions));
  EXPECT_EQ(fixture.removes, 0);
  EXPECT_EQ(fixture.adds, 0);

  // Reconciling replaces the item once, then does nothing.
  ASSERT_TRUE(observer.ReconcileItems(0, targets, definitions, functions));
  EXPECT_EQ(fixture.removes, 1);
  EXPECT_EQ(fixture.adds, 1);
  ASSERT_TRUE(observer.ReconcileItems(0, targets, definitions, functions));
  EXPECT_EQ(fixture.removes, 1);
  EXPECT_EQ(fixture.adds, 1);
  EXPECT_EQ(fixture.writes, 0);

  // The hero ability survives.
  auto owned = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(owned);
  ASSERT_EQ(owned->size(), 2u);
  EXPECT_EQ((*owned)[0].subclass_id, 100u);
}

TEST(PawnObserver, RejectsFalseItemDispatchAndLostConnection) {
  // Observe the fixture's loadout.
  ItemFixture fixture;
  PawnObserver observer(fixture.Seams());
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  const std::array targets{modlock::gameinterop::ItemTarget{201, 0x10001}};

  // A grant the engine does not apply fails after the removal.
  fixture.apply = false;
  EXPECT_FALSE(observer.ReconcileItems(0, targets, definitions, ItemFixture::Functions()));
  EXPECT_EQ(fixture.removes, 1);
  EXPECT_EQ(fixture.adds, 0);

  // A grant that does not appear in the vector fails too.
  fixture.SetCount(1);
  EXPECT_FALSE(observer.ReconcileItems(0, targets, definitions, ItemFixture::Functions()));
  EXPECT_EQ(fixture.adds, 1);  // A non-null returned pointer cannot certify a grant.

  // A disconnect during the removal stops before the grant.
  fixture.apply = true;
  fixture.SetCount(2);
  fixture.disconnect_on_remove = true;
  EXPECT_FALSE(observer.ReconcileItems(0, targets, definitions, ItemFixture::Functions()));
  EXPECT_EQ(fixture.adds, 1);  // No grant after an engine callback changes the session.
}

TEST(PawnObserver, ReconcilesMissingAuxiliaryAbilityThroughItsNativeOwner) {
  // Observe a pawn with no abilities.
  ItemFixture fixture;
  fixture.SetCount(0);
  PawnObserver observer{fixture.Seams()};
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  const std::array targets{modlock::gameinterop::AbilityUpgrade{100, 8, 65537}};

  // The native owner creates the hero ability in its slot.
  const auto create = +[](void* component, void* definition, uint16_t slot, uint64_t upgrade,
                          bool flag, void* extra) -> void* {
    // Check the creation's arguments.
    auto& fixture = *ItemFixture::current;
    EXPECT_EQ(component, fixture.pawn.data() + 0x48);
    EXPECT_EQ(definition, fixture.definition.data());
    EXPECT_EQ(slot, 8);
    EXPECT_EQ(upgrade, 0u);
    EXPECT_TRUE(flag);
    EXPECT_EQ(extra, nullptr);

    // Count it and create the ability when asked.
    ++fixture.adds;
    if (fixture.apply) {
      fixture.InitAbility(fixture.hero, 100, slot);
      fixture.SetCount(1);
    }
    return fixture.hero.data();
  };

  // A creation the engine does not apply fails.
  const auto set_bits = ItemFixture::Functions().set_bits;
  fixture.apply = false;
  EXPECT_FALSE(observer.ReconcileAbilities(0, targets, definitions, create, set_bits));

  // An applied creation restores the ability.
  fixture.apply = true;
  auto restored = observer.ReconcileAbilities(0, targets, definitions, create, set_bits);
  ASSERT_TRUE(restored) << restored.error();

  // It sits in its slot with its upgrade, after two attempts.
  const auto abilities = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(abilities);
  ASSERT_EQ(abilities->size(), 1u);
  EXPECT_EQ(abilities->front().slot, 8);
  EXPECT_EQ(abilities->front().upgrade_info, 65537u);
  EXPECT_EQ(fixture.adds, 2);

  // A second reconcile creates nothing.
  EXPECT_TRUE(observer.ReconcileAbilities(0, targets, definitions, create, set_bits));
  EXPECT_EQ(fixture.adds, 2);

  // A different subclass in the slot is refused.
  const std::array conflict{modlock::gameinterop::AbilityUpgrade{201, 8, 65537}};
  EXPECT_FALSE(observer.ReconcileAbilities(0, conflict, definitions, create, set_bits));
  EXPECT_EQ(fixture.adds, 2);

  // Abilities that share an unbound slot are left alone.
  fixture.InitAbility(fixture.hero, 100, 23);
  fixture.InitAbility(fixture.item, 101, 23);
  fixture.SetCount(2);
  const std::array unbound{modlock::gameinterop::AbilityUpgrade{100, 23, 65537},
                           modlock::gameinterop::AbilityUpgrade{101, 23, 65537}};
  EXPECT_TRUE(observer.ReconcileAbilities(0, unbound, definitions, create, set_bits));
  EXPECT_EQ(fixture.adds, 2);

  // The same definition in separate main slots retains independent upgrades.
  fixture.InitAbility(fixture.hero, 100, 0);
  fixture.InitAbility(fixture.item, 100, 1);
  const std::array repeated{modlock::gameinterop::AbilityUpgrade{100, 0, 65537},
                            modlock::gameinterop::AbilityUpgrade{100, 1, 196609}};
  auto copies = observer.ReconcileAbilities(0, repeated, definitions, create, set_bits);
  ASSERT_TRUE(copies) << copies.error();

  // Each copy keeps its own handle and upgrade, with no further creation.
  const auto independent = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(independent);
  ASSERT_EQ(independent->size(), 2u);
  EXPECT_NE(independent->at(0).handle, independent->at(1).handle);
  EXPECT_EQ(independent->at(0).upgrade_info, 65537u);
  EXPECT_EQ(independent->at(1).upgrade_info, 196609u);
  EXPECT_EQ(fixture.adds, 2);

  // A duplicate identity is still rejected before either copy changes.
  const std::array duplicate{repeated[0], repeated[0]};
  EXPECT_FALSE(observer.ReconcileAbilities(0, duplicate, definitions, create, set_bits));
}

TEST(PawnObserver, ObservesStaminaOnlyWhenFiniteAndPositive) {
  // Observe finite, positive stamina.
  ItemFixture fixture;
  PawnObserver observer{StaminaSeams(fixture)};
  fixture.connection.occupied = true;
  WriteStamina(fixture.pawn, 40.5f, 100.0f, 12.0f, 40.5f);

  // It reads back as written.
  auto sample = observer.Observe();
  ASSERT_TRUE(sample);
  ASSERT_TRUE(sample->stamina);
  EXPECT_FLOAT_EQ(sample->stamina->current, 40.5f);
  EXPECT_FLOAT_EQ(sample->stamina->max, 100.0f);

  // A non-finite current or a non-positive maximum is absence, not zero.
  WriteStamina(fixture.pawn, std::numeric_limits<float>::quiet_NaN(), 100.0f, 0, 0);
  sample = observer.Observe();
  ASSERT_TRUE(sample);
  EXPECT_FALSE(sample->stamina);

  // So does an infinite maximum.
  WriteStamina(fixture.pawn, 40.5f, std::numeric_limits<float>::infinity(), 0, 0);
  sample = observer.Observe();
  ASSERT_TRUE(sample);
  EXPECT_FALSE(sample->stamina);

  // And a zero maximum.
  WriteStamina(fixture.pawn, 40.5f, 0.0f, 0, 0);
  sample = observer.Observe();
  ASSERT_TRUE(sample);
  EXPECT_FALSE(sample->stamina);

  // Without the seam the capability does not exist at all.
  PawnObserver without{fixture.Seams()};
  WriteStamina(fixture.pawn, 40.5f, 100.0f, 0, 0);
  sample = without.Observe();
  ASSERT_TRUE(sample);
  EXPECT_FALSE(sample->stamina);
}

TEST(PawnObserver, SetsStaminaWithinNativeMaximumWithCallerTime) {
  // Observe a pawn with stamina and seeded regen bookkeeping.
  ItemFixture fixture;
  AttachNotifyTable(fixture);
  PawnObserver observer{StaminaSeams(fixture)};
  fixture.connection.occupied = true;
  WriteStamina(fixture.pawn, 10.0f, 100.0f, 3.25f, 10.0f);
  const float seeded_regen = 7.25f;
  std::memcpy(fixture.pawn.data() + kStaminaPrevRegenField, &seeded_regen, sizeof(seeded_regen));
  ASSERT_TRUE(observer.Observe());

  // A non-finite time is a caller defect before any write or notification.
  EXPECT_FALSE(observer.SetPracticeStamina(0, std::numeric_limits<float>::quiet_NaN()));
  float current = 0, latch_time = 0, latch_value = 0, max = 0;
  std::memcpy(&current, fixture.pawn.data() + kStaminaCurrentField, sizeof(current));
  EXPECT_FLOAT_EQ(current, 10.0f);
  std::memcpy(&latch_time, fixture.pawn.data() + kStaminaLatchTimeField, sizeof(latch_time));
  EXPECT_FLOAT_EQ(latch_time, 3.25f);

  // A stale connection is refused before any write.
  fixture.connection.occupied = false;
  EXPECT_FALSE(observer.SetPracticeStamina(0, 500.0f));
  std::memcpy(&current, fixture.pawn.data() + kStaminaCurrentField, sizeof(current));
  EXPECT_FLOAT_EQ(current, 10.0f);
  fixture.connection.occupied = true;

  // The refill mirrors HeroRefresh: current = max, latchValue = max,
  // latchTime = the caller's simulation seconds.
  auto restored = observer.SetPracticeStamina(0, 500.0f);
  ASSERT_TRUE(restored) << restored.error();
  std::memcpy(&current, fixture.pawn.data() + kStaminaCurrentField, sizeof(current));
  std::memcpy(&max, fixture.pawn.data() + kStaminaMaxField, sizeof(max));
  std::memcpy(&latch_value, fixture.pawn.data() + kStaminaLatchValueField, sizeof(latch_value));
  std::memcpy(&latch_time, fixture.pawn.data() + kStaminaLatchTimeField, sizeof(latch_time));

  // Current and latch value reach the maximum at the caller's time.
  EXPECT_FLOAT_EQ(current, 100.0f);
  EXPECT_FLOAT_EQ(latch_value, 100.0f);
  EXPECT_FLOAT_EQ(latch_time, 500.0f);

  // The native maximum itself is never rewritten.
  EXPECT_FLOAT_EQ(max, 100.0f);

  // The engine's regen bookkeeping survives the refill untouched.
  float regen = 0;
  std::memcpy(&regen, fixture.pawn.data() + kStaminaPrevRegenField, sizeof(regen));
  EXPECT_FLOAT_EQ(regen, 7.25f);

  // A chosen value drains the resource, and one past the maximum fills it.
  auto drained = observer.SetPracticeStamina(0, 502.0f, 0.0f);
  ASSERT_TRUE(drained) << drained.error();
  std::memcpy(&current, fixture.pawn.data() + kStaminaCurrentField, sizeof(current));
  std::memcpy(&latch_value, fixture.pawn.data() + kStaminaLatchValueField, sizeof(latch_value));
  EXPECT_FLOAT_EQ(current, 0.0f);
  EXPECT_FLOAT_EQ(latch_value, 0.0f);

  // A value past the maximum fills it.
  ASSERT_TRUE(observer.SetPracticeStamina(0, 503.0f, 250.0f));
  std::memcpy(&current, fixture.pawn.data() + kStaminaCurrentField, sizeof(current));
  EXPECT_FLOAT_EQ(current, 100.0f);

  // An unusable native maximum is refused before any write.
  WriteStamina(fixture.pawn, 10.0f, 0.0f, 0, 0);
  EXPECT_FALSE(observer.SetPracticeStamina(0, 501.0f));
  std::memcpy(&current, fixture.pawn.data() + kStaminaCurrentField, sizeof(current));
  EXPECT_FLOAT_EQ(current, 10.0f);
}

// The notification can replace the pawn: the controller adopts a different
// pawn handle and the image plants the new pawn at a fresh identity.
TEST(PawnObserver, StaminaRestoreRejectsPawnReplacedDuringTheNotification) {
  // Give the pawn a notification that replaces it with another pawn.
  ItemFixture fixture;
  auto seams = StaminaSeams(fixture);
  std::array<void*, 29> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) {
    // Bind the controller to a new pawn handle.
    auto& f = *ItemFixture::current;
    const uint32_t replacement = HandleOf(9, 3);
    f.connection.controller = MakeController(replacement);
    f.connection.image.SetIdentity(9, f.pawn.data(), 3, 0);
    std::memcpy(f.connection.controller.data() + kPawnHandleField, &replacement,
                sizeof(replacement));
  });
  WritePointer(fixture.pawn.data(), table.data());

  // Observe the pawn's stamina.
  PawnObserver observer{seams};
  fixture.connection.occupied = true;
  WriteStamina(fixture.pawn, 10.0f, 100.0f, 3.25f, 10.0f);
  ASSERT_TRUE(observer.Observe());

  // The notification swaps the pawn; the write must not report success.
  EXPECT_FALSE(observer.SetPracticeStamina(0, 500.0f));
}

TEST(PawnObserver, RestoresOwnedTimersWithReplicationAndReadback) {
  // Count each replication notification on both abilities.
  ItemFixture fixture;
  std::array<void*, 29> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) { ++ItemFixture::current->writes; });
  WritePointer(fixture.hero.data(), table.data());
  WritePointer(fixture.item.data(), table.data());

  // Observe the loadout and give each ability new timers.
  PawnObserver observer{fixture.Seams()};
  ASSERT_TRUE(observer.Observe());
  auto owned = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(owned) << owned.error();
  auto targets = *owned;
  for (auto& target : targets) {
    // Every target holds charges and running timers.
    target.charges = 2;
    target.cooldown_start = 100;
    target.cooldown_end = 120;
    target.charge_recharge_start = 105;
    target.charge_recharge_end = 115;
  }

  // A wrong handle writes nothing.
  auto invalid = targets;
  invalid.back().handle += 1;
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, invalid));
  EXPECT_EQ(fixture.writes, 0);

  // Neither does an infinite timer.
  invalid = targets;
  invalid.back().cooldown_end = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, invalid));
  EXPECT_EQ(fixture.writes, 0);

  // Nor a duplicate ability.
  invalid = {targets.front(), targets.front()};
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, invalid));
  EXPECT_EQ(fixture.writes, 0);

  // A valid batch notifies each ability once, then nothing.
  ASSERT_TRUE(observer.ApplyAbilityTimers(0, targets));
  EXPECT_EQ(fixture.writes, 2);
  ASSERT_TRUE(observer.ApplyAbilityTimers(0, targets));
  EXPECT_EQ(fixture.writes, 2);

  // The timers read back.
  owned = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned->front().charges, 2);
  EXPECT_EQ(owned->front().cooldown_start, 100);
  EXPECT_EQ(owned->back().charge_recharge_end, 115);
  EXPECT_EQ(owned->back().upgrade_info, 1u);

  // A notification that overwrites a timer fails the readback.
  table[28] = reinterpret_cast<void*>(+[](void* entity, const void*) {
    ++ItemFixture::current->writes;
    const float overwritten = 0;
    std::memcpy(static_cast<unsigned char*>(entity) + 0x44, &overwritten, sizeof(overwritten));
  });
  targets.front().cooldown_end = 130;
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, targets));
  EXPECT_EQ(fixture.writes, 3);

  // So does one that disconnects the player.
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) {
    ++ItemFixture::current->writes;
    ItemFixture::current->connection.occupied = false;
  });
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, targets));
  EXPECT_EQ(fixture.writes, 4);

  // After a reconnect, a notification that changes another ability's timer fails too.
  fixture.connection.occupied = true;
  ASSERT_TRUE(observer.Observe());
  table[28] = reinterpret_cast<void*>(+[](void* entity, const void*) {
    if (entity != ItemFixture::current->item.data()) {
      return;
    }
    const float overwritten = 0;
    std::memcpy(ItemFixture::current->hero.data() + 0x44, &overwritten, sizeof(overwritten));
  });
  targets.front().cooldown_end = 140;
  targets.back().cooldown_end = 150;
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, targets));
}

TEST(PawnObserver, ReconcilesActiveItemSlotsThroughEngineSwap) {
  // Observe a hero ability in slot 7 and an item in slot 4.
  ItemFixture fixture;
  fixture.InitAbility(fixture.hero, 201, 7);
  fixture.InitAbility(fixture.item, 200, 4);
  PawnObserver observer{fixture.Seams()};
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  auto functions = ItemFixture::Functions();

  // The engine swaps two slots when asked.
  functions.swap_slots = +[](void* component, uint16_t first, uint16_t second) {
    // Check and count the swap, then exchange the two slots when asked.
    auto& f = *ItemFixture::current;
    EXPECT_EQ(component, f.pawn.data() + 0x48);
    ++f.writes;
    if (!f.apply) {
      return;
    }
    for (auto* item : {&f.hero, &f.item}) {
      uint16_t slot;
      std::memcpy(&slot, item->data() + 0x38, sizeof(slot));
      if (slot == first) {
        slot = second;
      } else if (slot == second) {
        slot = first;
      }
      std::memcpy(item->data() + 0x38, &slot, sizeof(slot));
    }
  };

  // A target list with a duplicate slot writes nothing.
  const std::array targets{modlock::gameinterop::ItemTarget{200, 1, 7},
                           modlock::gameinterop::ItemTarget{201, 1, 4}};
  auto duplicate = targets;
  duplicate.back().slot = 7;
  EXPECT_FALSE(observer.ReconcileItems(0, duplicate, definitions, functions));
  EXPECT_EQ(fixture.writes, 0);

  // Reconciling swaps once, then does nothing.
  ASSERT_TRUE(observer.ReconcileItems(0, targets, definitions, functions));
  EXPECT_EQ(fixture.writes, 1);
  ASSERT_TRUE(observer.ReconcileItems(0, targets, definitions, functions));
  EXPECT_EQ(fixture.writes, 1);

  // The slots read back swapped.
  const auto owned = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned->front().slot, 4);
  EXPECT_EQ(owned->back().slot, 7);

  // A swap the engine does not apply fails.
  auto reversed = targets;
  reversed.front().slot = 4;
  reversed.back().slot = 7;
  fixture.apply = false;
  EXPECT_FALSE(observer.ReconcileItems(0, reversed, definitions, functions));
  EXPECT_EQ(fixture.writes, 2);
}

TEST(PawnObserver, FreshAbilityTargetsFullChargesAndClearedTimers) {
  // Observe a pawn whose abilities hold five charges.
  ItemFixture fixture;
  auto seams = fixture.Seams();
  seams.max_charges = [](void*) -> std::expected<int32_t, std::string> {
    return 5;
  };
  PawnObserver observer{seams};
  ASSERT_TRUE(observer.Observe());

  // Seed reduced charges and live timers on both owned abilities.
  const int32_t reduced_charges = 2;
  std::memcpy(fixture.hero.data() + 0x40, &reduced_charges, sizeof(reduced_charges));
  std::memcpy(fixture.item.data() + 0x40, &reduced_charges, sizeof(reduced_charges));
  const float cooldown = 30.0f, recharge = 12.5f;
  std::memcpy(fixture.hero.data() + 0x44, &cooldown, sizeof(cooldown));
  std::memcpy(fixture.item.data() + 0x44, &recharge, sizeof(recharge));

  // Fresh targets hold full charges and no timers.
  auto targets = observer.FreshAbilityTargets(0);
  ASSERT_TRUE(targets) << targets.error();
  ASSERT_EQ(targets->size(), 2u);
  for (const auto& target : *targets) {
    // Every target keeps its charges and clears its timers.
    EXPECT_EQ(target.charges, 5);
    EXPECT_FLOAT_EQ(target.cooldown_start, 0.0f);
    EXPECT_FLOAT_EQ(target.cooldown_end, 0.0f);
    EXPECT_FLOAT_EQ(target.charge_recharge_start, 0.0f);
    EXPECT_FLOAT_EQ(target.charge_recharge_end, 0.0f);
  }
  EXPECT_EQ((*targets)[0].subclass_id, 100u);
  EXPECT_EQ((*targets)[1].subclass_id, 200u);
  EXPECT_EQ(fixture.writes, 0);

  // The abilities themselves are unchanged.
  const auto unchanged = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(unchanged);
  for (const auto& ability : *unchanged) {
    EXPECT_EQ(ability.charges, reduced_charges);
  }
}

TEST(PawnObserver, FreshAbilityTargetsHonorsExplicitZeroAndRejectsReadFailure) {
  // Start from the fixture's seams.
  ItemFixture fixture;
  auto seams = fixture.Seams();

  // Native chargeless ability: the seam reports an explicit 0, not absence.
  seams.max_charges = [](void*) -> std::expected<int32_t, std::string> {
    return 0;
  };
  PawnObserver observer{seams};
  ASSERT_TRUE(observer.Observe());
  auto targets = observer.FreshAbilityTargets(0);
  ASSERT_TRUE(targets) << targets.error();
  ASSERT_EQ(targets->size(), 2u);
  for (const auto& target : *targets) {
    EXPECT_EQ(target.charges, 0);
    EXPECT_FLOAT_EQ(target.cooldown_end, 0.0f);
    EXPECT_FLOAT_EQ(target.charge_recharge_end, 0.0f);
  }

  // Unavailable charge maximum: the whole batch is refused, nothing partial.
  auto refusing = fixture.Seams();
  refusing.max_charges = [](void*) -> std::expected<int32_t, std::string> {
    return std::unexpected("native ability charge reader unavailable");
  };
  PawnObserver second{refusing};
  ASSERT_TRUE(second.Observe());
  auto refused = second.FreshAbilityTargets(0);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), "native ability charge reader unavailable");
}

TEST(PawnObserver, FreshAbilityTargetsRefusesWhenGetterMutatesOwnership) {
  // Start from the fixture's seams.
  ItemFixture fixture;
  auto seams = fixture.Seams();

  // A getter that changes the connection generation must poison the batch.
  seams.max_charges = [](void*) -> std::expected<int32_t, std::string> {
    ++ItemFixture::current->connection.generation;
    return 5;
  };
  PawnObserver observer{seams};
  ASSERT_TRUE(observer.Observe());
  auto refused = observer.FreshAbilityTargets(0);
  ASSERT_FALSE(refused);
}

}  // namespace

}  // namespace pawn_observer_test
