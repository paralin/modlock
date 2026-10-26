// Contract tests for the addressed pawn observer: slot occupancy from the
// game-clients seam, the controller at entity index slot+1 through the
// GetEntityIdentity chunk-table contract, the pawn handle resolved back
// through the serial proof, and session-generation rearming. The fixtures are
// recorded byte snapshots of the pinned sourcesdk layouts.
#include "modlock/gameinterop/pawn_observer.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "pawn_observer_fixture.h"

namespace pawn_observer_test {

namespace {

TEST(PawnObserver, ChargeQueriesFollowTheNativeRefreshCallsAcrossBuilds) {
  class Image final : public modlock::gameinterop::ModuleImage {
   public:
    uintptr_t base() const override { return 0x1000; }

    std::span<const uint8_t> image_bytes() const override { return bytes; }

    std::vector<uint8_t> bytes;
  } image;

  // Recorded HeroRefresh instructions from September 17's Windows server.dll.
  const std::vector<uint8_t> refresh{
      0xff, 0x90, 0xe8, 0x08, 0,    0,    0x84, 0xc0, 0x74, 0x17, 0x48, 0x8b, 0x1f, 0x48,
      0x8b, 0xcf, 0xff, 0x93, 0xb8, 0x09, 0,    0,    0x8b, 0xd0, 0x48, 0x8b, 0xcf, 0xff,
      0x93, 0xf8, 0x08, 0,    0,    0x45, 0x33, 0xd2, 0xff, 0xc5, 0x49, 0x83, 0xc6, 0x04};
  for (const size_t offset : {32, 96}) {
    // The current build's slots resolve wherever the call sits.
    image.bytes.assign(offset, 0);
    image.bytes.insert(image.bytes.end(), refresh.begin(), refresh.end());
    auto slots = modlock::gameinterop::ResolveAbilityChargeSlots(image);
    ASSERT_TRUE(slots) << slots.error();
    EXPECT_EQ(slots->has_charges, 285u);
    EXPECT_EQ(slots->max_charges, 311u);

    // The previous build used slots one entry earlier at a different address.
    image.bytes[offset + 2] -= 8;
    image.bytes[offset + 18] -= 8;
    slots = modlock::gameinterop::ResolveAbilityChargeSlots(image);
    ASSERT_TRUE(slots) << slots.error();
    EXPECT_EQ(slots->has_charges, 284u);
    EXPECT_EQ(slots->max_charges, 310u);
  }
}

TEST(PawnObserver, LiveHealthUsesThePawnVirtualFunction) {
  // Give a fake pawn a maximum health override at the slot the game uses.
  struct Pawn {
    void** table;
    int32_t maximum;
  };

  // Its table's override returns the stored maximum.
  std::array<void*, 182> table{};
  table[181] =
      reinterpret_cast<void*>(+[](void* pawn) { return static_cast<Pawn*>(pawn)->maximum; });
  Pawn pawn{table.data(), 730};

  // The live seam reads whatever the override returns.
  const auto read = PawnObserver::LiveSeams().effective_max_health;
  EXPECT_EQ(read(&pawn), 730);
  pawn.maximum = 1317;
  EXPECT_EQ(read(&pawn), 1317);
  pawn.maximum = 0;
  EXPECT_FALSE(read(&pawn));
}

TEST(PawnObserver, PrepareResolvesBeforeAnyPlayerArrives) {
  // Count layout resolutions and preparations with no player present.
  Fixture fixture;
  fixture.occupied = false;
  auto seams = SeamsFor(fixture);
  int layouts = 0;
  int prepared = 0;
  seams.layout = [&layouts]() -> std::expected<EntityLayout, std::string> {
    ++layouts;
    return FixtureLayout();
  };
  seams.prepare = [&prepared] {
    ++prepared;
  };

  // Prepare resolves the layout once.
  PawnObserver observer(seams);
  observer.Prepare();
  EXPECT_EQ(layouts, 1);
  EXPECT_EQ(prepared, 1);

  // The first observation of an occupied slot reuses the layout Prepare found.
  fixture.occupied = true;
  static_cast<void>(observer.Observe(fixture.slot));
  EXPECT_EQ(layouts, 1);
}

TEST(PawnObserver, ResetsStartingEconomyBeforeGrantingHigherLowerAndZeroBudgets) {
  // Seat a player with a controller and a hero pawn.
  Fixture fixture;
  fixture.occupied = true;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance pawn{}, body{}, scene{};
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  WritePointer(body.data() + kSceneNodeField, scene.data());

  // Give the pawn a hero and a team, and register both entities.
  const uint32_t hero = 14;
  std::memcpy(pawn.data() + kHeroField, &hero, sizeof(hero));
  pawn[kTeamField] = 2;
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);

  // Observe the pawn's wallet and abilities.
  auto seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    auto layout = FixtureLayout();
    layout.currencies = 0x80;
    return layout;
  };
  seams.ability_layout = []() -> std::expected<modlock::gameinterop::AbilityLayout, std::string> {
    return modlock::gameinterop::AbilityLayout{};
  };
  PawnObserver observer{std::move(seams)};

  // A reset leaves 500 souls, and each grant follows its reset.
  static unsigned resets, grants;
  resets = grants = 0;
  const auto reset = +[](void* entity, bool abilities) -> int64_t {
    // Reset the wallet to 500 souls.
    EXPECT_TRUE(abilities);
    ++resets;
    const std::array<int32_t, 3> starting{500, 0, 0};
    std::memcpy(static_cast<unsigned char*>(entity) + 0x80, starting.data(), sizeof(starting));
    return 0;
  };
  const auto grant = +[](void* entity, uint32_t type, int32_t amount, uint32_t source,
                         uint8_t silent, uint8_t force, uint8_t spend, void*, void*) {
    // Each grant follows its reset and is a silent, forced cheat grant.
    EXPECT_EQ(resets, ++grants);
    EXPECT_EQ(type, 0u);
    EXPECT_EQ(source, 7u);
    EXPECT_EQ(silent, 1);
    EXPECT_EQ(force, 1);
    EXPECT_EQ(spend, 0);

    // Apply the amount to the balance.
    auto* address = static_cast<unsigned char*>(entity) + 0x80;
    int32_t balance;
    std::memcpy(&balance, address, sizeof(balance));
    balance += amount;
    std::memcpy(address, &balance, sizeof(balance));
  };

  // Each budget resets the wallet, then grants up to it.
  for (int32_t souls : {20000, 40000, 10000, 0}) {
    auto result = observer.ResetStartingSouls(0, souls, grant, reset);
    ASSERT_TRUE(result) << result.error();
    EXPECT_EQ((*result->currencies)[0], souls);
    EXPECT_EQ((*result->currencies)[1], 0);
  }
  EXPECT_FALSE(observer.ResetStartingSouls(0, -1, grant, reset));
  EXPECT_EQ(resets, 4u);

  // A reset that changes the hero is refused before any grant.
  const auto changed_hero = +[](void* entity, bool) -> int64_t {
    const uint32_t hero = 15;
    std::memcpy(static_cast<unsigned char*>(entity) + kHeroField, &hero, sizeof(hero));
    return 0;
  };
  EXPECT_FALSE(observer.ResetStartingSouls(0, 20000, grant, changed_hero));
  EXPECT_EQ(grants, 4u);

  // So is a reset that reconnects the player.
  static Fixture* active_fixture;
  active_fixture = &fixture;
  const auto reconnect = +[](void*, bool) -> int64_t {
    ++active_fixture->generation;
    return 0;
  };
  EXPECT_FALSE(observer.ResetStartingSouls(0, 20000, grant, reconnect));
  EXPECT_EQ(grants, 4u);
  active_fixture = nullptr;
}

TEST(PawnObserver, ObservesRetainedDeadHeroAfterActivePawnBecomesSpectator) {
  // The controller's active pawn is a spectator while its hero handle names a dead hero.
  Fixture fixture;
  fixture.occupied = true;
  fixture.controller = MakeController(HandleOf(3, 8));
  fixture.controller[kTeamField] = 2;
  const auto hero_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));

  // Build the hero and the spectator.
  Instance hero{}, body{}, scene{}, spectator{};
  WritePointer(hero.data() + kBodyComponentField, body.data());
  WritePointer(body.data() + kSceneNodeField, scene.data());
  const uint32_t hero_id = 15;
  std::memcpy(hero.data() + kHeroField, &hero_id, sizeof(hero_id));
  hero[kTeamField] = 2;

  // Register the controller, hero and spectator.
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, hero.data(), 7, 0);
  fixture.image.SetIdentity(3, spectator.data(), 8, 0);
  PawnObserver observer{HeroSeamsFor(fixture)};

  // The observation reads the dead hero, not the spectator.
  const auto sample = observer.Observe();
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->pawn_handle, hero_handle);
  EXPECT_EQ(sample->hero_id, hero_id);
  EXPECT_EQ(sample->health, 0);
}

TEST(PawnObserver, ResolvesAddressedSlotZeroControllerAndPawn) {
  // Seat the player in slot zero.
  Fixture fixture;

  // The human's chain: controller at index 1, its pawn at index 2 serial 7.
  constexpr uint32_t kPawnSerial = 7;
  const uint32_t pawn_handle = HandleOf(2, kPawnSerial);
  fixture.controller = MakeController(pawn_handle);

  // Give the pawn an origin through its body and scene node.
  Instance pawn_body{};
  Instance pawn_scene{};
  Instance pawn{};
  float origin[3] = {1.5f, -2.25f, 300.0f};
  WritePointer(pawn_body.data() + kSceneNodeField, pawn_scene.data());
  std::memcpy(pawn_scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, pawn_body.data());

  // Give the pawn a hero and a team.
  uint32_t hero = 14;
  int32_t health = 800;
  std::memcpy(pawn.data() + kHeroField, &hero, sizeof(hero));
  pawn[kTeamField] = 2;

  // Set its angles and health.
  const std::array<float, 3> eye_angles{3.8671875f, 268.2422f, 0};
  std::memcpy(pawn.data() + 0x68, eye_angles.data(), sizeof(eye_angles));
  const std::array<float, 3> camera_angles{4.21875f, 270, 0};
  std::memcpy(pawn.data() + 0x74, camera_angles.data(), sizeof(camera_angles));
  std::memcpy(pawn.data() + kHealthField, &health, sizeof(health));

  // A non-controller neighbor with a nonzero pawn-handle-shaped word and its
  // own origin must never win selection.
  Instance decoy{};
  const uint32_t decoy_handle = HandleOf(9, 3);
  std::memcpy(decoy.data() + kPawnHandleField, &decoy_handle, sizeof(decoy_handle));

  // Give the decoy its own origin.
  const float decoy_origin[3] = {999.0f, 888.0f, 777.0f};
  Instance decoy_body{};
  Instance decoy_scene{};
  WritePointer(decoy_body.data() + kSceneNodeField, decoy_scene.data());
  std::memcpy(decoy_scene.data() + kAbsOriginField, decoy_origin, sizeof(decoy_origin));
  WritePointer(decoy.data() + kBodyComponentField, decoy_body.data());

  // Register the controller, pawn and decoy.
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), kPawnSerial, 0);
  fixture.image.SetIdentity(4, decoy.data(), 3, 0);

  // Give the pawn a wallet.
  const std::array<int32_t, 3> currencies{20000, 17, 2};
  std::memcpy(pawn.data() + 0x80, currencies.data(), sizeof(currencies));

  // The scoreboard lives on the owning controller at index 1, not on the
  // pawn; the fixture lays its counters out in ScoreTotals order.
  const modlock::gameinterop::ScoreTotals totals{45230, 8120, 3980, 21, 9, 17, 112, 14, 23650};
  std::memcpy(fixture.controller.data() + kPlayerDataGlobalField, &totals, sizeof(totals));

  // Observe through a layout with the wallet and scoreboard, and a max health getter that can fail.
  bool getter_available = true;
  auto seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    auto layout = FixtureLayout();
    layout.currencies = 0x80;
    layout.score = FixtureScore();
    return layout;
  };
  seams.ability_layout = []() -> std::expected<modlock::gameinterop::AbilityLayout, std::string> {
    return modlock::gameinterop::AbilityLayout{};
  };
  seams.effective_max_health = [&](void* observed_pawn) -> std::optional<int32_t> {
    EXPECT_EQ(observed_pawn, pawn.data());
    return getter_available ? std::optional<int32_t>{1317} : std::nullopt;
  };
  PawnObserver observer{std::move(seams)};

  // The first observation reads the wallet.
  fixture.occupied = true;
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  ASSERT_TRUE(sample->currencies);
  EXPECT_EQ(*sample->currencies, currencies);

  // Count the starting grants.
  static unsigned currency_calls;
  currency_calls = 0;
  const auto grant =
      +[](void* entity, uint32_t type, int32_t amount, uint32_t source, uint8_t silent,
          uint8_t force_gain, uint8_t spend_only, void* ability, void* source_entity) {
        // Each grant is a silent, forced cheat grant of souls.
        ++currency_calls;
        EXPECT_EQ(type, 0u);
        EXPECT_EQ(source, 7u);
        EXPECT_EQ(silent, 1);
        EXPECT_EQ(force_gain, 1);
        EXPECT_EQ(spend_only, 0);
        EXPECT_EQ(ability, nullptr);
        EXPECT_EQ(source_entity, nullptr);

        // Apply the amount to the balance.
        auto* address = static_cast<unsigned char*>(entity) + 0x80;
        int32_t current;
        std::memcpy(&current, address, sizeof(current));
        current += amount;
        std::memcpy(address, &current, sizeof(current));
      };

  // A wallet at the starting souls takes no grant; one below takes one.
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  EXPECT_EQ(currency_calls, 0u);
  int32_t balance = 18000;
  std::memcpy(pawn.data() + 0x80, &balance, sizeof(balance));
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  EXPECT_EQ(currency_calls, 1u);
  EXPECT_EQ((*observer.Observe()->currencies)[0], 20000);

  // A wallet above the starting souls keeps its balance.
  balance = 30000;
  std::memcpy(pawn.data() + 0x80, &balance, sizeof(balance));
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  EXPECT_EQ(currency_calls, 1u);
  EXPECT_EQ((*observer.Observe()->currencies)[0], 30000);

  // An empty wallet takes one grant, and a second call none.
  balance = 0;
  std::memcpy(pawn.data() + 0x80, &balance, sizeof(balance));
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  EXPECT_EQ(currency_calls, 2u);

  // Ability points and unlocks come from their own grants, once.
  const auto ability_grant = +[](void* entity, uint32_t type, int32_t amount, uint32_t source,
                                 uint8_t, uint8_t, uint8_t, void*, void*) {
    // Apply an ability point or unlock grant to its balance.
    EXPECT_TRUE(type == 1u || type == 2u);
    EXPECT_EQ(source, 7u);
    auto* address = static_cast<unsigned char*>(entity) + 0x80 + sizeof(int32_t) * type;
    int32_t balance;
    std::memcpy(&balance, address, sizeof(balance));
    balance += amount;
    std::memcpy(address, &balance, sizeof(balance));
  };
  auto prepared = observer.PrepareAbilityPoints(0, ability_grant);
  ASSERT_TRUE(prepared);
  EXPECT_EQ((*prepared->currencies)[0], 20000);
  EXPECT_EQ((*prepared->currencies)[1], 32);
  EXPECT_EQ((*prepared->currencies)[2], 4);
  ASSERT_TRUE(observer.PrepareAbilityPoints(0, ability_grant));

  // The sample carries the score, identity and health.
  ASSERT_TRUE(sample->score);
  EXPECT_EQ(*sample->score, totals);
  EXPECT_EQ(sample->slot, 0);
  EXPECT_EQ(sample->hero_id, 14);
  EXPECT_EQ(sample->team, 2);
  EXPECT_EQ(sample->health, 800);
  EXPECT_EQ(sample->effective_max_health, 1317);
  EXPECT_EQ(sample->max_health, 0);

  // A failed max health getter leaves the effective maximum absent.
  getter_available = false;
  auto unavailable = observer.Observe();
  ASSERT_TRUE(unavailable);
  EXPECT_FALSE(unavailable->effective_max_health);
  getter_available = true;

  // The sample carries the pose and the connection.
  EXPECT_EQ(sample->eye_angles, eye_angles);
  EXPECT_EQ(sample->camera_angles, camera_angles);
  EXPECT_EQ(sample->steam_id, fixture.xuid);
  EXPECT_EQ(sample->session_generation, 0u);
  EXPECT_EQ(sample->x, 1.5);
  EXPECT_EQ(sample->y, -2.25);
  EXPECT_EQ(sample->z, 300.0);

  // The located pawn and origin belong to slot zero only.
  EXPECT_EQ(observer.PawnForSlot(0), pawn.data());
  EXPECT_EQ(observer.PawnForSlot(1), nullptr);
  auto current = observer.CurrentOriginForSlot(0);
  ASSERT_TRUE(current.has_value());
  EXPECT_FLOAT_EQ((*current)[0], 1.5f);
  EXPECT_FLOAT_EQ((*current)[1], -2.25f);
  EXPECT_FLOAT_EQ((*current)[2], 300.0f);

  // A same-frame teleport updates the scene-node storage; the accessor must
  // reread that exact chain rather than retain the sample's old coordinates.
  origin[0] = 42.0f;
  origin[1] = -17.0f;
  origin[2] = 9.0f;
  std::memcpy(pawn_scene.data() + kAbsOriginField, origin, sizeof(origin));

  // The accessor reads the new origin.
  current = observer.CurrentOriginForSlot(0);
  ASSERT_TRUE(current.has_value());
  EXPECT_FLOAT_EQ((*current)[0], 42.0f);
  EXPECT_FLOAT_EQ((*current)[1], -17.0f);
  EXPECT_FLOAT_EQ((*current)[2], 9.0f);
  EXPECT_FALSE(observer.CurrentOriginForSlot(1).has_value());

  // A second client's controller is addressed by its own slot, and the first
  // slot no longer locates a pawn once its client leaves.
  fixture.slot = 7;
  fixture.image.SetIdentity(8, fixture.controller.data(), 5, 0);
  sample = observer.Observe(7);
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->slot, 7);
  EXPECT_EQ(sample->steam_id, fixture.xuid);

  // The totals follow the same owned controller/pawn identity as the pose.
  ASSERT_TRUE(sample->score);
  EXPECT_EQ(*sample->score, totals);
  EXPECT_EQ(observer.PawnForSlot(7), pawn.data());
  EXPECT_EQ(observer.PawnForSlot(0), nullptr);
  EXPECT_FALSE(observer.Observe(0));
  EXPECT_EQ(observer.PawnForSlot(7),
            pawn.data());  // A failed observation of another slot leaves this one.
}

TEST(PawnObserver, MissingScoreSchemaLeavesScoreAbsentAndPoseIntact) {
  // Seat a player with a controller.
  Fixture fixture;
  fixture.occupied = true;
  const uint32_t pawn_handle = HandleOf(2, 7);
  fixture.controller = MakeController(pawn_handle);

  // Give the pawn an origin and register both entities.
  Instance pawn{}, body{}, scene{};
  const float origin[3] = {5.0f, 6.0f, 7.0f};
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  WritePointer(body.data() + kSceneNodeField, scene.data());
  std::memcpy(scene.data() + kAbsOriginField, origin, sizeof(origin));
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);

  // The default fixture layout carries no scoreboard capability: the sample
  // still resolves, with the score absent rather than zero.
  PawnObserver observer{SeamsFor(fixture)};
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  EXPECT_FALSE(sample->score);
  EXPECT_EQ(sample->x, 5.0);
  EXPECT_EQ(sample->y, 6.0);
  EXPECT_EQ(sample->z, 7.0);
}

TEST(PawnObserver, OccupancyComesFromTheGameClientsSeamNotEntityBytes) {
  // Seat the player in slot zero.
  Fixture fixture;

  // Full controller and pawn chains exist, but the game clients seam reports
  // the slot empty: nothing may be selected from raw entity bytes alone.
  const uint32_t pawn_handle = HandleOf(2, 7);
  fixture.controller = MakeController(pawn_handle);

  // Give the pawn an origin.
  Instance pawn{};
  Instance pawn_body{};
  Instance pawn_scene{};
  const float origin[3] = {1.0f, 2.0f, 3.0f};
  WritePointer(pawn_body.data() + kSceneNodeField, pawn_scene.data());
  std::memcpy(pawn_scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, pawn_body.data());

  // Register both entities.
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);

  // Observing selects nothing.
  PawnObserver observer{SeamsFor(fixture)};
  EXPECT_FALSE(observer.Observe().has_value());
  EXPECT_EQ(observer.PawnForSlot(0), nullptr);
}

TEST(PawnObserver, SerialProofRejectsAMismatchedPawnHandle) {
  // Seat the player in slot zero.
  Fixture fixture;

  // The controller names pawn index 2 serial 8; the identity stores serial 7.
  fixture.controller = MakeController(HandleOf(2, 8));
  Instance pawn{};
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);

  // Observing rejects the pawn.
  PawnObserver observer{SeamsFor(fixture)};
  fixture.occupied = true;
  EXPECT_FALSE(observer.Observe().has_value());
}

TEST(PawnObserver, MissedFramesDoNotRearmDisconnectAndNewIdentityDo) {
  // Seat a player with a controller.
  Fixture fixture;
  const uint32_t pawn_handle = HandleOf(2, 7);
  fixture.controller = MakeController(pawn_handle);

  // Give the pawn an origin.
  Instance pawn{};
  Instance pawn_body{};
  Instance pawn_scene{};
  const float origin[3] = {1.0f, 2.0f, 3.0f};
  WritePointer(pawn_body.data() + kSceneNodeField, pawn_scene.data());
  std::memcpy(pawn_scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, pawn_body.data());

  // Register both entities.
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);

  // Observe an occupied slot.
  PawnObserver observer{SeamsFor(fixture)};
  fixture.occupied = true;

  // The first observation starts at generation zero.
  auto first = observer.Observe();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->session_generation, 0u);

  // The pawn drops out of the live chain while the slot stays occupied: no
  // sample and no generation change.
  fixture.image.SetIdentity(2, nullptr, 7, 0);
  EXPECT_FALSE(observer.Observe().has_value());
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);  // pawn returns next frame
  auto resumed = observer.Observe();
  ASSERT_TRUE(resumed.has_value());
  EXPECT_EQ(resumed->session_generation, 0u);

  // Confirmed disconnect rearms.
  fixture.occupied = false;
  EXPECT_FALSE(observer.Observe().has_value());
  fixture.occupied = true;
  fixture.generation = 41;
  auto reconnected = observer.Observe();
  ASSERT_TRUE(reconnected.has_value());
  EXPECT_EQ(reconnected->session_generation, 41u);
  EXPECT_EQ(reconnected->steam_id, first->steam_id);

  // A different xuid taking the still-occupied slot rearms again.
  fixture.xuid = 2000;
  fixture.generation = 99;
  auto replaced = observer.Observe();
  ASSERT_TRUE(replaced.has_value());
  EXPECT_EQ(replaced->session_generation, 99u);
  EXPECT_EQ(replaced->steam_id, 2000u);
}

TEST(PawnObserver, DirectMovementFieldsAreFiniteAndGroundProvenanceIsExplicit) {
  // Seat a player with a pawn and modifiers.
  Fixture fixture;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance pawn{}, body{}, scene{}, modifiers{};
  WritePointer(pawn.data() + 0xa0, modifiers.data());

  // Give the pawn a position, a velocity and no ground.
  const float origin[3] = {1, 2, 3};
  const float velocity[3] = {100, -2, 0.5f};
  const uint32_t ground = 0xffffffff;
  WritePointer(body.data() + kSceneNodeField, scene.data());
  std::memcpy(scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  std::memcpy(pawn.data() + kAbsVelocityField, velocity, sizeof(velocity));
  std::memcpy(pawn.data() + kGroundEntityField, &ground, sizeof(ground));

  // Register them and observe movement with dash modifiers.
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);
  fixture.occupied = true;
  PawnObserver::Seams seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    auto layout = MovementFixtureLayout();
    layout.movement->modifier_property = 0xa0;
    layout.movement->modifier_state_mask = 0;
    return layout;
  };
  PawnObserver observer{std::move(seams), true};

  // The velocity reads back.
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  ASSERT_TRUE(sample->movement.has_value());
  ASSERT_TRUE(sample->movement->abs_velocity.has_value());
  EXPECT_FLOAT_EQ((*sample->movement->abs_velocity)[0], 100);

  // The ground is explicitly none.
  ASSERT_TRUE(sample->movement->ground_entity_handle.has_value());
  ASSERT_TRUE(sample->movement->grounded_by_handle.has_value());
  EXPECT_EQ(*sample->movement->ground_entity_handle, 0xffffffffu);
  EXPECT_FALSE(*sample->movement->grounded_by_handle);

  // Each dash modifier but 0x23 reads as dashing.
  for (uint32_t flag : {0x23u, 0x25u, 0x29u}) {
    // Every flag but 0x23 reads as a dash.
    const uint32_t mask = uint32_t{1} << (flag % 32);
    std::memcpy(modifiers.data() + 4, &mask, sizeof(mask));
    const auto moving = observer.Observe();
    ASSERT_TRUE(moving && moving->movement);
    EXPECT_EQ(moving->movement->dashing, flag != 0x23u);
  }
  WritePointer(pawn.data() + 0xa0, nullptr);
  EXPECT_FALSE(observer.Observe()->movement->dashing);

  // The default observer path does not touch optional movement fields.
  auto disabled_seams = SeamsFor(fixture);
  disabled_seams.layout = []() -> std::expected<EntityLayout, std::string> {
    return MovementFixtureLayout();
  };
  PawnObserver disabled{std::move(disabled_seams)};
  auto disabled_sample = disabled.Observe();
  ASSERT_TRUE(disabled_sample.has_value());
  EXPECT_FALSE(disabled_sample->movement.has_value());
}

TEST(PawnObserver, NonfiniteVelocityIsAbsentRatherThanReconstructed) {
  // Seat a player with a pawn.
  Fixture fixture;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance pawn{}, body{}, scene{};

  // Give the pawn a position and a velocity that is not finite.
  const float origin[3] = {1, 2, 3};
  const float velocity[3] = {100, std::numeric_limits<float>::quiet_NaN(), 0.5f};
  WritePointer(body.data() + kSceneNodeField, scene.data());
  std::memcpy(scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  std::memcpy(pawn.data() + kAbsVelocityField, velocity, sizeof(velocity));

  // Register them and observe movement.
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);
  fixture.occupied = true;
  PawnObserver::Seams seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    return MovementFixtureLayout();
  };
  PawnObserver observer{std::move(seams), true};

  // The velocity is absent.
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  ASSERT_TRUE(sample->movement.has_value());
  EXPECT_FALSE(sample->movement->abs_velocity.has_value());
}

TEST(PawnObserver, MissingMovementFieldsRemainAbsentWithoutHidingIdentity) {
  // Seat a player with a positioned pawn.
  Fixture fixture;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance pawn{}, body{}, scene{};
  const float origin[3] = {1, 2, 3};
  WritePointer(body.data() + kSceneNodeField, scene.data());
  std::memcpy(scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, body.data());

  // Observe through a layout without velocity or ground.
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);
  fixture.occupied = true;
  PawnObserver::Seams seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    return MovementFixtureLayout(false, false);
  };
  PawnObserver observer{std::move(seams), true};

  // Identity reads while the movement fields are absent.
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  EXPECT_EQ(sample->steam_id, fixture.xuid);
  ASSERT_TRUE(sample->movement.has_value());
  EXPECT_FALSE(sample->movement->abs_velocity.has_value());
  EXPECT_FALSE(sample->movement->ground_entity_handle.has_value());
  EXPECT_FALSE(sample->movement->grounded_by_handle.has_value());
}

}  // namespace

}  // namespace pawn_observer_test
