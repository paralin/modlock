// Contract tests for the addressed pawn observer: slot occupancy from the
// game-clients seam, the controller at entity index slot+1 through the
// GetEntityIdentity chunk-table contract, the pawn handle resolved back
// through the serial proof, and session-generation rearming. The fixtures are
// recorded byte snapshots of the pinned sourcesdk layouts.
#include "modlock/gameinterop/pawn_observer.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace {

using modlock::gameinterop::EntityLayout;
using modlock::gameinterop::MovementLayout;
using modlock::gameinterop::PawnObserver;

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

// Pinned CEntityIdentity layout (sourcesdk public/entity2/entityidentity.h,
// "Size: 0x70").
constexpr size_t kIdentityStride = 0x70;
constexpr size_t kIdentityInstanceOffset = 0x00;
constexpr size_t kIdentityEHandleOffset = 0x10;
constexpr size_t kIdentityFlagsOffset = 0x30;
constexpr size_t kIdentityBackPointer = 0x10;  // CEntityInstance::m_pEntity
constexpr uint32_t kMaxEntitiesInList = 512;   // MAX_ENTITIES_IN_LIST
constexpr int kMaxEntityLists = 64;            // MAX_ENTITY_LISTS

// Fake field offsets inside the fixture entity instances. Independent of any
// production constant on purpose: a drift between them fails resolution.
constexpr size_t kPawnHandleField = 0x40;
constexpr size_t kBodyComponentField = 0x20;
constexpr size_t kSceneNodeField = 0x08;
constexpr size_t kAbsOriginField = 0x40;
constexpr size_t kAbsVelocityField = 0x80;
constexpr size_t kGroundEntityField = 0x90;
constexpr size_t kHeroField = 0x50;
constexpr size_t kTeamField = 0x54;
constexpr size_t kHealthField = 0x58;
constexpr size_t kHeroHandleField = 0x48;
constexpr size_t kPlayStateField = 0x60;
// Fake controller-side scoreboard: PlayerDataGlobal_t inline in the
// controller, then its int32 counters in ScoreTotals order. Independent of
// any production constant on purpose.
constexpr size_t kPlayerDataGlobalField = 0x80;

uint32_t HandleOf(uint32_t index, uint32_t serial) {
  return (index & 0x7FFF) | ((serial & 0x1FFFF) << 15);
}

void WritePointer(void* at, void* value) {
  std::memcpy(at, &value, sizeof(value));
}

// EntityImage builds the layout one live CGameEntitySystem carries:
// m_EntityList sits at +0x10 with its m_pIdentityChunks[64] table inline, and
// each chunk points at a contiguous array of 512 CEntityIdentity records.
class EntityImage {
 public:
  EntityImage() : chunk_(std::make_unique<unsigned char[]>(kIdentityStride * kMaxEntitiesInList)) {
    std::memset(chunk_.get(), 0, kIdentityStride * kMaxEntitiesInList);
    // vptr and m_pCurrentManifest precede the inline chunk table.
    system_.assign(0x10 + sizeof(void*) * (kMaxEntityLists + 4), 0);
    unsigned char* chunk = chunk_.get();
    std::memcpy(system_.data() + 0x10, &chunk, sizeof(chunk));
  }

  // System returns the fake CGameEntitySystem pointer.
  void* System() { return system_.data(); }

  // SetIdentity plants an identity record at entity index pointing back at
  // instance through m_pEntity.
  unsigned char* SetIdentity(uint32_t index, void* instance, uint32_t serial, uint32_t flags) {
    // Write the identity's instance, handle and flags, and point the instance back at it.
    unsigned char* identity = chunk_.get() + index * kIdentityStride;
    WritePointer(identity + kIdentityInstanceOffset, instance);
    const uint32_t ehandle = HandleOf(index, serial);
    std::memcpy(identity + kIdentityEHandleOffset, &ehandle, sizeof(ehandle));
    std::memcpy(identity + kIdentityFlagsOffset, &flags, sizeof(flags));
    if (instance != nullptr) {
      WritePointer(static_cast<unsigned char*>(instance) + kIdentityBackPointer, identity);
    }
    return identity;
  }

 private:
  std::vector<unsigned char> system_;
  std::unique_ptr<unsigned char[]> chunk_;
};

// Instance is one fixture entity object with room for every fake field.
using Instance = std::array<unsigned char, 0x100>;

Instance MakeController(uint32_t pawn_handle) {
  Instance controller{};
  std::memcpy(controller.data() + kPawnHandleField, &pawn_handle, sizeof(pawn_handle));
  return controller;
}

struct Fixture {
  EntityImage image;
  Instance controller{MakeController(0)};
  Instance decoy{};
  bool occupied = false;
  int32_t slot = 0;
  // The engine-reported xuid the lifecycle seam returns while occupied.
  uint64_t xuid = 1000;
  uint32_t generation = 0;
};

modlock::gameinterop::ScoreLayout FixtureScore() {
  return {.player_data_global = kPlayerDataGlobalField,
          .hero_damage = 0x00,
          .hero_healing = 0x04,
          .self_healing = 0x08,
          .kills = 0x0c,
          .deaths = 0x10,
          .assists = 0x14,
          .last_hits = 0x18,
          .denies = 0x1c,
          .net_worth = 0x20};
}

EntityLayout FixtureLayout() {
  return EntityLayout{.pawn_handle = kPawnHandleField,
                      .body_component = kBodyComponentField,
                      .scene_node = kSceneNodeField,
                      .abs_origin = kAbsOriginField,
                      .hero_id = kHeroField,
                      .team = kTeamField,
                      .health = kHealthField,
                      .max_health = kHealthField + 4,
                      .level = kHealthField + 8,
                      .eye_angles = 0x68,
                      .camera_angles = 0x74};
}

EntityLayout MovementFixtureLayout(bool velocity = true, bool ground = true) {
  // Add the requested movement fields to the fixture layout.
  EntityLayout layout = FixtureLayout();
  MovementLayout movement;
  if (velocity) {
    movement.abs_velocity = kAbsVelocityField;
  }
  if (ground) {
    movement.ground_entity = kGroundEntityField;
  }
  layout.movement = movement;
  return layout;
}

PawnObserver::Seams SeamsFor(Fixture& fixture) {
  // Serve the fixture's entity system, slot and layout.
  PawnObserver::Seams seams;
  seams.entity_system = [&fixture]() -> std::expected<void*, std::string> {
    return fixture.image.System();
  };
  seams.slot_state = [&fixture](int32_t slot) {
    if (slot != fixture.slot || !fixture.occupied) {
      return modlock::gameinterop::ConnectionTracker::SlotState{};
    }
    return modlock::gameinterop::ConnectionTracker::SlotState{
        .occupied = true, .xuid = fixture.xuid, .generation = fixture.generation};
  };
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    return FixtureLayout();
  };
  return seams;
}

// HeroSeamsFor also locates the controller's hero handle, which player
// selection and respawn need.
PawnObserver::Seams HeroSeamsFor(Fixture& fixture) {
  auto seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    auto layout = FixtureLayout();
    layout.hero_pawn_handle = kHeroHandleField;
    return layout;
  };
  return seams;
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

  // The frame's pawn and origin belong to slot zero only.
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

  // A second client's controller is addressed by its own slot, without
  // borrowing the previous client's frame-scoped mutation capability.
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
  EXPECT_EQ(observer.PawnForSlot(7), nullptr);
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
    EXPECT_EQ(changed.error(), "controller pawn changed during this frame");
  }

  // Restore the pawn.
  std::memcpy(fixture.controller.data() + kPawnHandleField, &pawn_handle, sizeof(pawn_handle));
  changing_controller = nullptr;

  // A reconnect expires the frame until the next observation.
  ++fixture.generation;
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  ASSERT_TRUE(observer.Observe());
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

// Engine callback fixture over the same entity/handle memory used by the observer.
struct ItemFixture {
  Fixture connection;
  Instance pawn{}, body{}, scene{}, hero{}, item{}, granted_item{};
  std::array<uint32_t, 4> handles{};
  std::array<unsigned char, 0x30> definition{};
  int adds = 0, removes = 0, writes = 0;
  bool apply = true;
  bool disconnect_on_remove = false;
  static ItemFixture* current;

  ItemFixture() {
    // Seat the controller and give the pawn its body and scene.
    current = this;
    connection.occupied = true;
    connection.controller = MakeController(HandleOf(2, 7));
    WritePointer(pawn.data() + kBodyComponentField, body.data());
    WritePointer(body.data() + kSceneNodeField, scene.data());

    // Register the controller, the pawn and both abilities.
    connection.image.SetIdentity(1, connection.controller.data(), 1, 0);
    connection.image.SetIdentity(2, pawn.data(), 7, 0);
    connection.image.SetIdentity(3, hero.data(), 9, 0);
    connection.image.SetIdentity(4, item.data(), 10, 0);

    // Give the pawn a hero ability and an item, in a vector with room for four.
    InitAbility(hero, 100, 0);
    InitAbility(item, 200, 23);
    handles[0] = HandleOf(3, 9);
    handles[1] = HandleOf(4, 10);
    SetCount(2);
    WritePointer(pawn.data() + 0xc8, handles.data());
    const int32_t capacity = 4;
    std::memcpy(pawn.data() + 0xd0, &capacity, sizeof(capacity));
  }
  ~ItemFixture() { current = nullptr; }
  void SetCount(int32_t count) { std::memcpy(pawn.data() + 0xc0, &count, sizeof(count)); }
  void InitAbility(Instance& entity, uint32_t id, uint16_t slot) {
    // Owned by the pawn, with its subclass, slot and packed upgrade.
    const uint32_t owner = HandleOf(2, 7), packed = 1;
    std::memcpy(entity.data() + 0x30, &owner, 4);
    std::memcpy(entity.data() + 0x34, &id, 4);
    std::memcpy(entity.data() + 0x38, &slot, 2);
    std::memcpy(entity.data() + 0x3c, &packed, 4);
  }
  PawnObserver::Seams Seams() {
    auto seams = SeamsFor(connection);
    seams.ability_layout = []() -> std::expected<modlock::gameinterop::AbilityLayout, std::string> {
      return modlock::gameinterop::AbilityLayout{.component = 0x48,
                                                 .handles = 0xc0,
                                                 .owner = 0x30,
                                                 .subclass = 0x34,
                                                 .slot = 0x38,
                                                 .upgrade_info = 0x3c,
                                                 .charges = 0x40,
                                                 .cooldown_end = 0x44,
                                                 .cooldown_start = 0x48,
                                                 .charge_recharge_start = 0x4c,
                                                 .charge_recharge_end = 0x50};
    };
    return seams;
  }
  static void* Lookup(int32_t scope, uint32_t id) {
    // Name the three known subclasses in scope 4.
    EXPECT_EQ(scope, 4);
    const char* name = id == 100   ? "bull_heal"
                       : id == 200 ? "upgrade_old"
                       : id == 201 ? "upgrade_new"
                                   : nullptr;
    if (!name) {
      return nullptr;
    }
    std::memcpy(current->definition.data() + 8, &scope, 4);
    std::memcpy(current->definition.data() + 16, &name, sizeof(name));
    return current->definition.data();
  }
  static modlock::gameinterop::ItemFunctions Functions() {
    return {+[](void* pawn, const char* name, uint64_t upgrade, void* extra) -> void* {
              // Check the grant's arguments, count it, and apply it when asked.
              auto& f = *current;
              EXPECT_EQ(pawn, f.pawn.data());
              EXPECT_STREQ(name, "upgrade_new");
              EXPECT_EQ(extra, nullptr);
              ++f.adds;
              if (f.apply) {
                f.InitAbility(f.item, 201, 4);
                // The upgrade's low word holds the bits; its high dword the
                // packed low word.
                const uint32_t packed =
                    (static_cast<uint32_t>(upgrade & 0xffff) << 16) | uint32_t(upgrade >> 32);
                std::memcpy(f.item.data() + 0x3c, &packed, 4);
                f.handles[1] = HandleOf(4, 10);
                f.SetCount(2);
              }
              return f.item.data();
            },
            +[](void* component, void* item, uint8_t flag) {
              // Check the removal's arguments, count it, and apply it when asked.
              auto& f = *current;
              EXPECT_EQ(component, f.pawn.data() + 0x48);
              EXPECT_EQ(item, f.item.data());
              EXPECT_EQ(flag, 0);
              ++f.removes;
              if (f.apply) {
                f.SetCount(1);
              }
              if (f.disconnect_on_remove) {
                ++f.connection.generation;
              }
            },
            +[](void* item, uint32_t bits) {
              auto& f = *current;
              ++f.writes;
              if (f.apply) {
                std::memcpy(static_cast<unsigned char*>(item) + 0x3e, &bits, 2);
              }
            }};
  }
};
ItemFixture* ItemFixture::current = nullptr;

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

// Stamina fixture offsets: the resource floats sit behind the inline ability
// component. Independent of any production constant on purpose. The pawn
// instance has room for 0xa0 bytes; the floats occupy its tail.
constexpr size_t kStaminaCurrentField = 0x80;
constexpr size_t kStaminaMaxField = 0x84;
constexpr size_t kStaminaLatchTimeField = 0x88;
constexpr size_t kStaminaLatchValueField = 0x8c;
constexpr size_t kStaminaPrevRegenField = 0x90;

modlock::gameinterop::StaminaLayout StaminaFixtureLayout() {
  return {.current = kStaminaCurrentField,
          .max = kStaminaMaxField,
          .latch_time = kStaminaLatchTimeField,
          .latch_value = kStaminaLatchValueField};
}

// The observer reads and restores stamina through the resolved EntityLayout;
// the seam composes the fixture offsets with the shared fixture layout.
PawnObserver::Seams StaminaSeams(ItemFixture& fixture) {
  auto seams = fixture.Seams();
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    auto layout = FixtureLayout();
    layout.stamina = StaminaFixtureLayout();
    return layout;
  };
  return seams;
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
}

// NotifyFullState requires the pinned vtable slot 27 on the notified entity.
// The lambda cannot capture; it reaches the fixture through the static
// current pointer the same way the item reconciliation tests do.
void AttachNotifyTable(ItemFixture& fixture) {
  static std::array<void*, 29> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) { ++ItemFixture::current->writes; });
  WritePointer(fixture.pawn.data(), table.data());
}

void WriteStamina(Instance& pawn, float current, float max, float latch_time, float latch_value) {
  std::memcpy(pawn.data() + kStaminaCurrentField, &current, sizeof(current));
  std::memcpy(pawn.data() + kStaminaMaxField, &max, sizeof(max));
  std::memcpy(pawn.data() + kStaminaLatchTimeField, &latch_time, sizeof(latch_time));
  std::memcpy(pawn.data() + kStaminaLatchValueField, &latch_value, sizeof(latch_value));
}

}  // namespace

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

TEST(PawnObserver, PlayerSelectionStopsAfterTeamCallbackLosesConnection) {
  // Seat a player whose team change bumps the connection generation.
  static Fixture* active = nullptr;
  static int observer_spawns = 0;
  Fixture fixture;
  fixture.occupied = true;
  fixture.generation = 7;
  active = &fixture;
  observer_spawns = 0;

  // Install the team change in the controller's table.
  std::array<void*, 106> table{};
  table[105] = reinterpret_cast<void*>(+[](void*, int) { ++active->generation; });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);

  // Count observer spawns.
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.spawn_observer = +[](void*) -> void* {
    ++observer_spawns;
    return nullptr;
  };
  calls.set_pawn = +[](void*, void*, bool, bool, bool, bool) {
  };

  // Selection stops when the team change loses the connection.
  PawnObserver observer{HeroSeamsFor(fixture)};
  const auto refused = observer.SelectPlayer(0, fixture.xuid, 7, 1, nullptr, calls);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().find("connection changed"), std::string::npos);
  EXPECT_EQ(observer_spawns, 0);
  active = nullptr;
}

TEST(PawnObserver, SpectatorSelectionNeedsNoHeroAndDoesNotCreateCombatPawn) {
  // Seat a player already on the spectator team.
  static int observer_spawns = 0;
  static Fixture* active = nullptr;
  observer_spawns = 0;
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 1;

  // Fail on any team change.
  std::array<void*, 106> table{};
  table[105] =
      reinterpret_cast<void*>(+[](void*, int) { ADD_FAILURE() << "already on spectator team"; });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);

  // Spawn the observer without publishing a pawn, and fail on any pawn bind.
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.spawn_observer = +[](void*) -> void* {
    ++observer_spawns;
    // The engine can return an actor before publishing m_hPawn.
    return active->controller.data();
  };
  calls.set_pawn = +[](void*, void*, bool, bool, bool, bool) {
    ADD_FAILURE() << "already on spectator team";
  };

  // Selection waits for the pawn and spawns once.
  PawnObserver observer{HeroSeamsFor(fixture)};
  EXPECT_FALSE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  EXPECT_FALSE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  EXPECT_EQ(observer_spawns, 1);

  // Once the same connection publishes a pawn, selection becomes ready and
  // later observer checks must reuse it rather than creating another actor.
  auto pawn = MakeController(0);
  fixture.image.SetIdentity(2, pawn.data(), 9, 0);
  const uint32_t handle = HandleOf(2, 9);
  std::memcpy(fixture.controller.data() + kPawnHandleField, &handle, sizeof(handle));
  EXPECT_TRUE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  EXPECT_TRUE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  EXPECT_EQ(observer_spawns, 1);

  // Another player's identity is refused.
  EXPECT_FALSE(observer.SelectPlayer(0, fixture.xuid + 1, 3, 1, nullptr, calls));
  EXPECT_EQ(observer_spawns, 1);
}

TEST(PawnObserver, SpectatorSelectionReleasesTheHeroBeforeTheTeamChanges) {
  // Seat a player on team 2.
  static Fixture* active = nullptr;
  static std::vector<std::string> steps;
  Fixture fixture;
  active = &fixture;
  steps.clear();
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 2;

  // Bind them to a hero.
  Instance hero{};
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, hero.data(), 7, 0);
  const auto hero_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  std::memcpy(fixture.controller.data() + kPawnHandleField, &hero_handle, sizeof(hero_handle));

  // The controller records each native call in order.
  std::array<void*, 106> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) { steps.push_back("notify"); });
  table[105] = reinterpret_cast<void*>(+[](void*, int team) {
    EXPECT_EQ(team, 1);
    steps.push_back("team");
  });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.set_pawn = +[](void* controller, void* pawn, bool retain_old_pawn_team, bool, bool, bool) {
    // Unbind the pawn and keep its team.
    EXPECT_EQ(controller, active->controller.data());
    EXPECT_EQ(pawn, nullptr);
    EXPECT_TRUE(retain_old_pawn_team);
    const uint32_t none = 0xffffffff;
    std::memcpy(static_cast<char*>(controller) + kPawnHandleField, &none, sizeof(none));
    steps.push_back("unbind");
  };
  calls.spawn_observer = +[](void*) -> void* {
    steps.push_back("observer");
    return nullptr;
  };

  // The hero handle is cleared and published, and the pawn unbound, before
  // the team changes and the observer spawns.
  PawnObserver observer{HeroSeamsFor(fixture)};
  EXPECT_FALSE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  uint32_t released = 0;
  std::memcpy(&released, fixture.controller.data() + kHeroHandleField, sizeof(released));
  EXPECT_EQ(released, 0xffffffffu);
  EXPECT_EQ(steps, (std::vector<std::string>{"notify", "unbind", "team", "observer"}));
  active = nullptr;
}

PawnObserver::Seams SeatingSeamsFor(Fixture& fixture, bool play_state = true) {
  auto seams = HeroSeamsFor(fixture);
  seams.layout = [play_state]() -> std::expected<EntityLayout, std::string> {
    auto layout = FixtureLayout();
    layout.hero_pawn_handle = kHeroHandleField;
    if (play_state) {
      layout.play_state = kPlayStateField;
    }
    return layout;
  };
  return seams;
}

TEST(PawnObserver, SeatingJoinsATeamlessPlayerAndLeavesThemChoosingAHero) {
  // Seat a teamless player.
  static Fixture* active = nullptr;
  static std::vector<std::string> steps;
  Fixture fixture;
  active = &fixture;
  steps.clear();
  fixture.occupied = true;
  fixture.generation = 3;

  // Record each notification's play state and each team change.
  std::array<void*, 106> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) {
    int32_t state = 0;
    std::memcpy(&state, active->controller.data() + kPlayStateField, sizeof(state));
    steps.push_back("notify:" + std::to_string(state));
  });
  table[105] = reinterpret_cast<void*>(+[](void* controller, int team) {
    static_cast<unsigned char*>(controller)[kTeamField] = static_cast<unsigned char>(team);
    steps.push_back("team:" + std::to_string(team));
  });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);

  // The player has no hero, and creating one records its team.
  const uint32_t none = 0xffffffff;
  std::memcpy(fixture.controller.data() + kHeroHandleField, &none, sizeof(none));
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.create_pawn = +[](void*, int team) -> void* {
    steps.push_back("pawn:" + std::to_string(team));
    return nullptr;
  };

  // Seating joins the team.
  PawnObserver observer{SeatingSeamsFor(fixture)};
  EXPECT_EQ(observer.ControllerTeam(0), 0);
  const auto seated = observer.SeatForChoice(0, fixture.xuid, 3, 3, calls);
  ASSERT_TRUE(seated) << seated.error();
  EXPECT_TRUE(*seated);

  // The game's own join: the team, the hero pawn, then the play state, which
  // is EPlayerChoosingHero when it replicates.
  EXPECT_EQ(steps, (std::vector<std::string>{"team:3", "pawn:3", "notify:2"}));
  EXPECT_EQ(observer.ControllerTeam(0), 3);

  // A player who has a team is left alone.
  steps.clear();
  const auto again = observer.SeatForChoice(0, fixture.xuid, 3, 2, calls);
  ASSERT_TRUE(again) << again.error();
  EXPECT_FALSE(*again);
  EXPECT_TRUE(steps.empty());
  active = nullptr;
}

TEST(PawnObserver, SeatingRefusesWithoutThePlayStateAndLeavesTheTeamScreen) {
  // Seat a teamless player.
  static Fixture* active = nullptr;
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;

  // Fail on any team change.
  std::array<void*, 106> table{};
  table[105] = reinterpret_cast<void*>(+[](void*, int) { ADD_FAILURE() << "team changed"; });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.create_pawn = +[](void*, int) -> void* {
    return nullptr;
  };

  // Without the play state seating is refused and the team stays.
  PawnObserver observer{SeatingSeamsFor(fixture, false)};
  const auto refused = observer.SeatForChoice(0, fixture.xuid, 3, 2, calls);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().find("play state"), std::string::npos);
  EXPECT_EQ(observer.ControllerTeam(0), 0);
  EXPECT_FALSE(observer.SeatForChoice(0, fixture.xuid, 3, 1, calls));
  active = nullptr;
}

TEST(PawnObserver, HeroSelectionDoesNotTreatObserverPawnAsHeroPawn) {
  // Track the hero pawn and the native calls.
  static Fixture* active = nullptr;
  static void* hero_pawn = nullptr;
  static int created = 0;
  static int selected = 0;

  // Seat a player on team 2.
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 2;

  // Fail on any team change.
  std::array<void*, 106> table{};
  table[105] = reinterpret_cast<void*>(+[](void*, int) { ADD_FAILURE(); });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);

  // Bind the player to a spectator pawn; the hero pawn is separate.
  auto spectator = MakeController(0);
  auto hero = MakeController(0);
  hero_pawn = hero.data();
  fixture.image.SetIdentity(2, spectator.data(), 7, 0);
  fixture.image.SetIdentity(3, hero.data(), 8, 0);
  const auto spectator_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kPawnHandleField, &spectator_handle,
              sizeof(spectator_handle));

  // Creating the hero publishes its handle, and selection checks it.
  modlock::gameinterop::PlayerSelectionCalls calls;
  created = selected = 0;
  calls.create_pawn = +[](void*, int) -> void* {
    ++created;
    const auto handle = HandleOf(3, 8);
    std::memcpy(active->controller.data() + kHeroHandleField, &handle, sizeof(handle));
    return hero_pawn;
  };
  calls.select_hero = +[](void* pawn, void*) {
    EXPECT_EQ(pawn, hero_pawn);
    ++selected;
  };

  // Selection creates and selects the hero once.
  PawnObserver observer{HeroSeamsFor(fixture)};
  EXPECT_TRUE(observer.SelectPlayer(0, fixture.xuid, 3, 2, &fixture, calls));
  EXPECT_EQ(created, 1);
  EXPECT_EQ(selected, 1);
}

TEST(PawnObserver, WatchingKeepsTheHeroAndReturnsToIt) {
  // Track the camera, the rebound pawn and the spawns.
  static Fixture* active = nullptr;
  static void* camera_pawn = nullptr;
  static void* rebound = nullptr;
  static int spawns = 0;

  // Seat a player on team 2.
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 2;

  // Give them a living hero and a camera.
  Instance hero{}, camera{};
  camera_pawn = camera.data();
  const int32_t health = 800;
  std::memcpy(hero.data() + kHealthField, &health, sizeof(health));

  // Register them and bind the hero.
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, hero.data(), 7, 0);
  fixture.image.SetIdentity(3, camera.data(), 8, 0);
  const auto hero_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  std::memcpy(fixture.controller.data() + kPawnHandleField, &hero_handle, sizeof(hero_handle));

  // The game binds the camera it spawns, and the hero it is given.
  modlock::gameinterop::PlayerSelectionCalls calls;
  spawns = 0;
  rebound = nullptr;
  calls.spawn_observer = +[](void*) -> void* {
    ++spawns;
    const auto handle = HandleOf(3, 8);
    std::memcpy(active->controller.data() + kPawnHandleField, &handle, sizeof(handle));
    return camera_pawn;
  };
  calls.set_pawn = +[](void* controller, void* pawn, bool retain_old_pawn_team, bool, bool, bool) {
    EXPECT_FALSE(retain_old_pawn_team);
    rebound = pawn;
    const auto handle = HandleOf(2, 7);
    std::memcpy(static_cast<char*>(controller) + kPawnHandleField, &handle, sizeof(handle));
  };

  // The hero stays the player's hero while a single camera watches.
  PawnObserver observer{HeroSeamsFor(fixture)};
  EXPECT_EQ(observer.CameraPawnForSlot(0, calls).value_or(nullptr), camera.data());
  EXPECT_EQ(observer.CameraPawnForSlot(0, calls).value_or(nullptr), camera.data());
  EXPECT_EQ(spawns, 1);
  uint32_t kept = 0;
  std::memcpy(&kept, fixture.controller.data() + kHeroHandleField, sizeof(kept));
  EXPECT_EQ(kept, hero_handle);
  EXPECT_EQ(observer.WatchingSlots(), std::vector<int32_t>{0});

  // Returning binds the hero and hands back the camera to remove, once.
  EXPECT_EQ(observer.ReturnToHero(0, calls).value_or(0), HandleOf(3, 8));
  EXPECT_EQ(rebound, hero.data());
  EXPECT_EQ(observer.ReturnToHero(0, calls).value_or(1), 0u);
  EXPECT_TRUE(observer.WatchingSlots().empty());

  // A camera still bound after the player reconnects, or after the runtime
  // forgot it, returns the player to their living hero all the same.
  const auto camera_handle = HandleOf(3, 8);
  std::memcpy(fixture.controller.data() + kPawnHandleField, &camera_handle, sizeof(camera_handle));
  fixture.generation = 4;
  rebound = nullptr;
  PawnObserver restarted{HeroSeamsFor(fixture)};
  EXPECT_EQ(restarted.ReturnToHero(0, calls).value_or(0), camera_handle);
  EXPECT_EQ(rebound, hero.data());
  active = nullptr;
}

TEST(PawnObserver, ReturnLeavesTheGamesCameraAfterTheHeroDies) {
  // Track the camera and seat a player on team 3.
  static Fixture* active = nullptr;
  static void* camera_pawn = nullptr;
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 3;

  // Give them a hero, a camera and a death camera.
  Instance hero{}, camera{}, death{};
  camera_pawn = camera.data();
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, hero.data(), 7, 0);
  fixture.image.SetIdentity(3, camera.data(), 8, 0);
  fixture.image.SetIdentity(4, death.data(), 9, 0);

  // Bind the hero.
  const auto hero_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  std::memcpy(fixture.controller.data() + kPawnHandleField, &hero_handle, sizeof(hero_handle));

  // Spawning binds the camera; the dead hero cannot be bound.
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.spawn_observer = +[](void*) -> void* {
    const auto handle = HandleOf(3, 8);
    std::memcpy(active->controller.data() + kPawnHandleField, &handle, sizeof(handle));
    return camera_pawn;
  };
  calls.set_pawn = +[](void*, void*, bool, bool, bool, bool) {
    ADD_FAILURE() << "hero is dead";
  };

  // Watch through the camera.
  PawnObserver observer{HeroSeamsFor(fixture)};
  ASSERT_TRUE(observer.CameraPawnForSlot(0, calls));

  // The hero dies and the game binds its own camera, which a later watch
  // reuses; the respawn returns the player to the hero.
  const auto death_handle = HandleOf(4, 9);
  std::memcpy(fixture.controller.data() + kPawnHandleField, &death_handle, sizeof(death_handle));
  EXPECT_EQ(observer.ReturnToHero(0, calls).value_or(0), HandleOf(3, 8));
  EXPECT_EQ(observer.CameraPawnForSlot(0, calls).value_or(nullptr), death.data());
  EXPECT_TRUE(observer.WatchingSlots().empty());
  active = nullptr;
}

TEST(PawnObserver, RespawnUsesDeadHeroHandleAndRejectsStaleConnections) {
  // Track the respawned pawn and the respawns.
  static void* expected_pawn = nullptr;
  static int respawned = 0;

  // Seat a player on team 3 with a spectator and a hero.
  Fixture fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance spectator{}, hero{};
  expected_pawn = hero.data();
  respawned = 0;

  // Register them and name the hero.
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, spectator.data(), 7, 0);
  fixture.image.SetIdentity(3, hero.data(), 8, 0);
  auto hero_handle = HandleOf(3, 8);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  PawnObserver observer{HeroSeamsFor(fixture)};

  // Respawning restores the hero's health.
  auto respawn = +[](void* pawn, bool force) {
    // Check the respawn and give the pawn health.
    EXPECT_EQ(pawn, expected_pawn);
    EXPECT_TRUE(force);
    ++respawned;
    const int32_t health = 100;
    std::memcpy(static_cast<char*>(pawn) + kHealthField, &health, sizeof(health));
  };

  // The wrong team respawns nothing.
  EXPECT_FALSE(observer.RespawnPlayer(0, 2, respawn));
  EXPECT_EQ(respawned, 0);

  // The dead hero respawns once.
  ASSERT_TRUE(observer.RespawnPlayer(0, 3, respawn));
  EXPECT_EQ(respawned, 1);
  ASSERT_TRUE(observer.RespawnPlayer(0, 3, respawn));
  EXPECT_EQ(respawned, 1);

  // A stale hero handle is refused.
  hero_handle = HandleOf(3, 9);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  EXPECT_FALSE(observer.RespawnPlayer(0, 3, respawn));
  EXPECT_EQ(respawned, 1);
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
