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
  struct Pawn {
    void** table;
    int32_t maximum;
  };
  std::array<void*, 182> table{};
  table[181] =
      reinterpret_cast<void*>(+[](void* pawn) { return static_cast<Pawn*>(pawn)->maximum; });
  Pawn pawn{table.data(), 730};
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
// Fake controller-side combat totals: PlayerDataGlobal_t inline in the
// controller, then its int32 counters. Independent of any production
// constant on purpose.
constexpr size_t kPlayerDataGlobalField = 0x80;
constexpr size_t kHeroDamageField = 0x00;
constexpr size_t kHeroHealingField = 0x04;
constexpr size_t kSelfHealingField = 0x08;
constexpr size_t kKillsField = 0x0c;
constexpr size_t kDeathsField = 0x10;
constexpr size_t kAssistsField = 0x14;

uint32_t HandleOf(uint32_t index, uint32_t serial) {
  return (index & 0x7FFF) | ((serial & 0x1FFFF) << 15);
}

void WritePointer(void* at, void* value) { std::memcpy(at, &value, sizeof(value)); }

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

modlock::gameinterop::CombatLayout FixtureCombat() {
  return {.player_data_global = kPlayerDataGlobalField,
          .hero_damage = kHeroDamageField,
          .hero_healing = kHeroHealingField,
          .self_healing = kSelfHealingField,
          .kills = kKillsField,
          .deaths = kDeathsField,
          .assists = kAssistsField};
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
  EntityLayout layout = FixtureLayout();
  MovementLayout movement;
  if (velocity) movement.abs_velocity = kAbsVelocityField;
  if (ground) movement.ground_entity = kGroundEntityField;
  layout.movement = movement;
  return layout;
}

PawnObserver::Seams SeamsFor(Fixture& fixture) {
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
  seams.layout = []() -> std::expected<EntityLayout, std::string> { return FixtureLayout(); };
  return seams;
}

TEST(PawnObserver, ResetsStartingEconomyBeforeGrantingHigherLowerAndZeroBudgets) {
  Fixture fixture;
  fixture.occupied = true;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance pawn{}, body{}, scene{};
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  WritePointer(body.data() + kSceneNodeField, scene.data());
  const uint32_t hero = 14;
  std::memcpy(pawn.data() + kHeroField, &hero, sizeof(hero));
  pawn[kTeamField] = 2;
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);
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
  static unsigned resets, grants;
  resets = grants = 0;
  const auto reset = +[](void* entity, bool abilities) -> int64_t {
    EXPECT_TRUE(abilities);
    ++resets;
    const std::array<int32_t, 3> starting{500, 0, 0};
    std::memcpy(static_cast<unsigned char*>(entity) + 0x80, starting.data(), sizeof(starting));
    return 0;
  };
  const auto grant = +[](void* entity, uint32_t type, int32_t amount, uint32_t source,
                         uint8_t silent, uint8_t force, uint8_t spend, void*, void*) {
    EXPECT_EQ(resets, ++grants);
    EXPECT_EQ(type, 0u);
    EXPECT_EQ(source, 7u);
    EXPECT_EQ(silent, 1);
    EXPECT_EQ(force, 1);
    EXPECT_EQ(spend, 0);
    auto* address = static_cast<unsigned char*>(entity) + 0x80;
    int32_t balance;
    std::memcpy(&balance, address, sizeof(balance));
    balance += amount;
    std::memcpy(address, &balance, sizeof(balance));
  };
  for (int32_t souls : {20000, 40000, 10000, 0}) {
    auto result = observer.ResetStartingSouls(0, souls, grant, reset);
    ASSERT_TRUE(result) << result.error();
    EXPECT_EQ((*result->currencies)[0], souls);
    EXPECT_EQ((*result->currencies)[1], 0);
  }
  EXPECT_FALSE(observer.ResetStartingSouls(0, -1, grant, reset));
  EXPECT_EQ(resets, 4u);
  const auto changed_hero = +[](void* entity, bool) -> int64_t {
    const uint32_t hero = 15;
    std::memcpy(static_cast<unsigned char*>(entity) + kHeroField, &hero, sizeof(hero));
    return 0;
  };
  EXPECT_FALSE(observer.ResetStartingSouls(0, 20000, grant, changed_hero));
  EXPECT_EQ(grants, 4u);
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
  constexpr size_t kHeroHandleField = 0x48;
  Fixture fixture;
  fixture.occupied = true;
  fixture.controller = MakeController(HandleOf(3, 8));
  fixture.controller[kTeamField] = 2;
  const auto hero_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  Instance hero{}, body{}, scene{}, spectator{};
  WritePointer(hero.data() + kBodyComponentField, body.data());
  WritePointer(body.data() + kSceneNodeField, scene.data());
  const uint32_t hero_id = 15;
  std::memcpy(hero.data() + kHeroField, &hero_id, sizeof(hero_id));
  hero[kTeamField] = 2;
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, hero.data(), 7, 0);
  fixture.image.SetIdentity(3, spectator.data(), 8, 0);
  auto seams = SeamsFor(fixture);
  seams.layout = [kHeroHandleField]() -> std::expected<EntityLayout, std::string> {
    auto layout = FixtureLayout();
    layout.hero_pawn_handle = kHeroHandleField;
    return layout;
  };
  PawnObserver observer{std::move(seams)};
  const auto sample = observer.Observe();
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->pawn_handle, hero_handle);
  EXPECT_EQ(sample->hero_id, hero_id);
  EXPECT_EQ(sample->health, 0);
}

TEST(PawnObserver, ResolvesAddressedSlotZeroControllerAndPawn) {
  Fixture fixture;
  // The human's chain: controller at index 1, its pawn at index 2 serial 7.
  constexpr uint32_t kPawnSerial = 7;
  const uint32_t pawn_handle = HandleOf(2, kPawnSerial);
  fixture.controller = MakeController(pawn_handle);
  Instance pawn_body{};
  Instance pawn_scene{};
  Instance pawn{};
  float origin[3] = {1.5f, -2.25f, 300.0f};
  WritePointer(pawn_body.data() + kSceneNodeField, pawn_scene.data());
  std::memcpy(pawn_scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, pawn_body.data());

  uint32_t hero = 14;
  int32_t health = 800;
  std::memcpy(pawn.data() + kHeroField, &hero, sizeof(hero));
  pawn[kTeamField] = 2;
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
  const float decoy_origin[3] = {999.0f, 888.0f, 777.0f};
  Instance decoy_body{};
  Instance decoy_scene{};
  WritePointer(decoy_body.data() + kSceneNodeField, decoy_scene.data());
  std::memcpy(decoy_scene.data() + kAbsOriginField, decoy_origin, sizeof(decoy_origin));
  WritePointer(decoy.data() + kBodyComponentField, decoy_body.data());

  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), kPawnSerial, 0);
  fixture.image.SetIdentity(4, decoy.data(), 3, 0);

  const std::array<int32_t, 3> currencies{20000, 17, 2};
  std::memcpy(pawn.data() + 0x80, currencies.data(), sizeof(currencies));
  // Combat totals live on the owning controller at index 1, not on the pawn.
  const modlock::gameinterop::CombatTotals totals{45230, 8120, 3980, 21, 9, 17};
  const auto write_totals = [&](unsigned char* controller) {
    std::memcpy(controller + kPlayerDataGlobalField + kHeroDamageField, &totals.hero_damage,
                sizeof(totals.hero_damage));
    std::memcpy(controller + kPlayerDataGlobalField + kHeroHealingField, &totals.hero_healing,
                sizeof(totals.hero_healing));
    std::memcpy(controller + kPlayerDataGlobalField + kSelfHealingField, &totals.self_healing,
                sizeof(totals.self_healing));
    std::memcpy(controller + kPlayerDataGlobalField + kKillsField, &totals.kills,
                sizeof(totals.kills));
    std::memcpy(controller + kPlayerDataGlobalField + kDeathsField, &totals.deaths,
                sizeof(totals.deaths));
    std::memcpy(controller + kPlayerDataGlobalField + kAssistsField, &totals.assists,
                sizeof(totals.assists));
  };
  write_totals(fixture.controller.data());
  bool getter_available = true;
  auto seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    auto layout = FixtureLayout();
    layout.currencies = 0x80;
    layout.combat = FixtureCombat();
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
  fixture.occupied = true;
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  ASSERT_TRUE(sample->currencies);
  EXPECT_EQ(*sample->currencies, currencies);
  static unsigned currency_calls;
  currency_calls = 0;
  const auto grant =
      +[](void* entity, uint32_t type, int32_t amount, uint32_t source, uint8_t silent,
          uint8_t force_gain, uint8_t spend_only, void* ability, void* source_entity) {
        ++currency_calls;
        EXPECT_EQ(type, 0u);
        EXPECT_EQ(source, 7u);
        EXPECT_EQ(silent, 1);
        EXPECT_EQ(force_gain, 1);
        EXPECT_EQ(spend_only, 0);
        EXPECT_EQ(ability, nullptr);
        EXPECT_EQ(source_entity, nullptr);
        auto* address = static_cast<unsigned char*>(entity) + 0x80;
        int32_t current;
        std::memcpy(&current, address, sizeof(current));
        current += amount;
        std::memcpy(address, &current, sizeof(current));
      };
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  EXPECT_EQ(currency_calls, 0u);
  int32_t balance = 18000;
  std::memcpy(pawn.data() + 0x80, &balance, sizeof(balance));
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  EXPECT_EQ(currency_calls, 1u);
  EXPECT_EQ((*observer.Observe()->currencies)[0], 20000);
  balance = 30000;
  std::memcpy(pawn.data() + 0x80, &balance, sizeof(balance));
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  EXPECT_EQ(currency_calls, 1u);
  EXPECT_EQ((*observer.Observe()->currencies)[0], 30000);
  balance = 0;
  std::memcpy(pawn.data() + 0x80, &balance, sizeof(balance));
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  ASSERT_TRUE(observer.PrepareStartingSouls(0, 20000, grant));
  EXPECT_EQ(currency_calls, 2u);
  const auto ability_grant = +[](void* entity, uint32_t type, int32_t amount, uint32_t source,
                                 uint8_t, uint8_t, uint8_t, void*, void*) {
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

  ASSERT_TRUE(sample->combat_totals);
  EXPECT_EQ(*sample->combat_totals, totals);
  EXPECT_EQ(sample->slot, 0);
  EXPECT_EQ(sample->hero_id, 14);
  EXPECT_EQ(sample->team, 2);
  EXPECT_EQ(sample->health, 800);
  EXPECT_EQ(sample->effective_max_health, 1317);
  EXPECT_EQ(sample->max_health, 0);
  getter_available = false;
  auto unavailable = observer.Observe();
  ASSERT_TRUE(unavailable);
  EXPECT_FALSE(unavailable->effective_max_health);
  getter_available = true;
  EXPECT_EQ(sample->eye_angles, eye_angles);
  EXPECT_EQ(sample->camera_angles, camera_angles);
  EXPECT_EQ(sample->steam_id, fixture.xuid);
  EXPECT_EQ(sample->session_generation, 0u);
  EXPECT_EQ(sample->x, 1.5);
  EXPECT_EQ(sample->y, -2.25);
  EXPECT_EQ(sample->z, 300.0);
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
  ASSERT_TRUE(sample->combat_totals);
  EXPECT_EQ(*sample->combat_totals, totals);
  EXPECT_EQ(observer.PawnForSlot(7), pawn.data());
  EXPECT_EQ(observer.PawnForSlot(0), nullptr);
  EXPECT_FALSE(observer.Observe(0));
  EXPECT_EQ(observer.PawnForSlot(7), nullptr);
}

TEST(PawnObserver, MissingCombatSchemaLeavesTotalsAbsentAndPoseIntact) {
  Fixture fixture;
  fixture.occupied = true;
  const uint32_t pawn_handle = HandleOf(2, 7);
  fixture.controller = MakeController(pawn_handle);
  Instance pawn{}, body{}, scene{};
  const float origin[3] = {5.0f, 6.0f, 7.0f};
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  WritePointer(body.data() + kSceneNodeField, scene.data());
  std::memcpy(scene.data() + kAbsOriginField, origin, sizeof(origin));
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);

  // The default fixture layout carries no combat capability: the sample
  // still resolves, with the totals absent rather than zero.
  PawnObserver observer{SeamsFor(fixture)};
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  EXPECT_FALSE(sample->combat_totals);
  EXPECT_EQ(sample->x, 5.0);
  EXPECT_EQ(sample->y, 6.0);
  EXPECT_EQ(sample->z, 7.0);
}

TEST(PawnObserver, OccupancyComesFromTheGameClientsSeamNotEntityBytes) {
  Fixture fixture;
  // Full controller and pawn chains exist, but the game clients seam reports
  // the slot empty: nothing may be selected from raw entity bytes alone.
  const uint32_t pawn_handle = HandleOf(2, 7);
  fixture.controller = MakeController(pawn_handle);
  Instance pawn{};
  Instance pawn_body{};
  Instance pawn_scene{};
  const float origin[3] = {1.0f, 2.0f, 3.0f};
  WritePointer(pawn_body.data() + kSceneNodeField, pawn_scene.data());
  std::memcpy(pawn_scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, pawn_body.data());
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);

  PawnObserver observer{SeamsFor(fixture)};
  EXPECT_FALSE(observer.Observe().has_value());
  EXPECT_EQ(observer.PawnForSlot(0), nullptr);
}

TEST(PawnObserver, SerialProofRejectsAMismatchedPawnHandle) {
  Fixture fixture;
  // The controller names pawn index 2 serial 8; the identity stores serial 7.
  fixture.controller = MakeController(HandleOf(2, 8));
  Instance pawn{};
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);

  PawnObserver observer{SeamsFor(fixture)};
  fixture.occupied = true;
  EXPECT_FALSE(observer.Observe().has_value());
}

TEST(PawnObserver, MissedFramesDoNotRearmDisconnectAndNewIdentityDo) {
  Fixture fixture;
  const uint32_t pawn_handle = HandleOf(2, 7);
  fixture.controller = MakeController(pawn_handle);
  Instance pawn{};
  Instance pawn_body{};
  Instance pawn_scene{};
  const float origin[3] = {1.0f, 2.0f, 3.0f};
  WritePointer(pawn_body.data() + kSceneNodeField, pawn_scene.data());
  std::memcpy(pawn_scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, pawn_body.data());
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);

  PawnObserver observer{SeamsFor(fixture)};
  fixture.occupied = true;

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
  Fixture fixture;
  fixture.occupied = true;
  const uint32_t pawn_handle = HandleOf(2, 7);
  fixture.controller = MakeController(pawn_handle);
  Instance pawn{}, body{}, scene{}, ability{};
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  WritePointer(body.data() + kSceneNodeField, scene.data());
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);
  fixture.image.SetIdentity(3, ability.data(), 9, 0);
  const uint32_t ability_handle = HandleOf(3, 9);
  uint32_t handles[] = {0xFFFFFFFF, ability_handle, ability_handle};
  const int32_t count = 2;
  const uint32_t capacity = 3;
  std::memcpy(pawn.data() + 0xc0, &count, sizeof(count));
  WritePointer(pawn.data() + 0xc8, handles);
  std::memcpy(pawn.data() + 0xd0, &capacity, sizeof(capacity));
  std::memcpy(ability.data() + 0x30, &pawn_handle, sizeof(pawn_handle));
  const uint32_t subclass = 0xf1234567;
  const uint16_t slot = 12;
  ability[0x3A] = 2;
  std::memcpy(ability.data() + 0x34, &subclass, sizeof(subclass));
  std::memcpy(ability.data() + 0x38, &slot, sizeof(slot));
  const uint32_t upgrade_info = 0x70001;
  const int32_t charges = 2;
  const float cooldown = 123.25f;
  const float cooldown_start = 100.5f, recharge_start = 105.75f, recharge_end = 110.25f;
  std::memcpy(ability.data() + 0x48, &cooldown_start, sizeof(cooldown_start));
  std::memcpy(ability.data() + 0x4c, &recharge_start, sizeof(recharge_start));
  std::memcpy(ability.data() + 0x50, &recharge_end, sizeof(recharge_end));
  std::memcpy(ability.data() + 0x3c, &upgrade_info, sizeof(upgrade_info));
  std::memcpy(ability.data() + 0x40, &charges, sizeof(charges));
  std::memcpy(ability.data() + 0x44, &cooldown, sizeof(cooldown));
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
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  const auto observed = observer.Observe();
  ASSERT_TRUE(observed);
  ASSERT_TRUE(observed->movement);
  EXPECT_EQ(observed->movement->jump_ability_handle, ability_handle);
  EXPECT_EQ(observed->movement->consecutive_wall_jumps, 2);
  auto actual = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(actual) << actual.error();
  ASSERT_EQ(actual->size(), 1u);
  EXPECT_EQ(actual->front().handle, ability_handle);
  EXPECT_EQ(actual->front().subclass_id, subclass);
  EXPECT_EQ(actual->front().slot, slot);
  EXPECT_EQ(actual->front().consecutive_wall_jumps, 2);
  ability[0x3A] = 0xFF;
  const auto invalid_count = observer.Observe();
  ASSERT_TRUE(invalid_count);
  EXPECT_FALSE(invalid_count->movement->consecutive_wall_jumps);
  ability[0x3A] = 0;
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(1));
  EXPECT_EQ(actual->front().upgrade_info, upgrade_info);
  EXPECT_EQ(actual->front().charges, charges);
  EXPECT_EQ(actual->front().cooldown_end, cooldown);
  EXPECT_EQ(actual->front().cooldown_start, cooldown_start);
  EXPECT_EQ(actual->front().charge_recharge_start, recharge_start);
  EXPECT_EQ(actual->front().charge_recharge_end, recharge_end);
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
  static int writes = 0;
  writes = 0;
  const auto set_bits = +[](void* entity, uint32_t bits) {
    ++writes;
    std::memcpy(static_cast<unsigned char*>(entity) + 0x3e, &bits, sizeof(uint16_t));
  };
  const modlock::gameinterop::AbilityUpgrade target{subclass, slot, 0xf0001};
  const std::array invalid_targets{target, modlock::gameinterop::AbilityUpgrade{123, 2, 0x10001}};
  EXPECT_FALSE(observer.ApplyAbilityUpgrades(0, invalid_targets, set_bits));
  EXPECT_EQ(writes, 0);  // Validate the whole request before touching the first ability.
  const std::array targets{target};
  ASSERT_TRUE(observer.ApplyAbilityUpgrades(0, targets, set_bits));
  EXPECT_EQ(writes, 1);
  ASSERT_TRUE(observer.ApplyAbilityUpgrades(0, targets, set_bits));
  EXPECT_EQ(writes, 1);  // An already restored mask has no second engine side effect.
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
  if (!changed) EXPECT_EQ(changed.error(), "controller pawn changed during this frame");
  std::memcpy(fixture.controller.data() + kPawnHandleField, &pawn_handle, sizeof(pawn_handle));
  changing_controller = nullptr;

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
  const uint32_t wrong_owner = HandleOf(2, 6);
  std::memcpy(ability.data() + 0x30, &wrong_owner, sizeof(wrong_owner));
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  std::memcpy(ability.data() + 0x30, &pawn_handle, sizeof(pawn_handle));
  const int32_t duplicate_count = 3;
  std::memcpy(pawn.data() + 0xc0, &duplicate_count, sizeof(duplicate_count));
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  Instance second_ability = ability;
  fixture.image.SetIdentity(4, second_ability.data(), 10, 0);
  handles[2] = HandleOf(4, 10);
  const auto ambiguous = observer.Observe();
  ASSERT_TRUE(ambiguous);
  ASSERT_TRUE(ambiguous->movement);
  EXPECT_FALSE(ambiguous->movement->jump_ability_handle);
  EXPECT_FALSE(ambiguous->movement->consecutive_wall_jumps);
  const int32_t oversized_count = 257;
  std::memcpy(pawn.data() + 0xc0, &oversized_count, sizeof(oversized_count));
  EXPECT_FALSE(observer.CurrentAbilitiesForSlot(0));
  const int32_t empty_count = 0;
  std::memcpy(pawn.data() + 0xc0, &empty_count, sizeof(empty_count));
  actual = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(actual);
  EXPECT_TRUE(actual->empty());
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
    current = this;
    connection.occupied = true;
    connection.controller = MakeController(HandleOf(2, 7));
    WritePointer(pawn.data() + kBodyComponentField, body.data());
    WritePointer(body.data() + kSceneNodeField, scene.data());
    connection.image.SetIdentity(1, connection.controller.data(), 1, 0);
    connection.image.SetIdentity(2, pawn.data(), 7, 0);
    connection.image.SetIdentity(3, hero.data(), 9, 0);
    connection.image.SetIdentity(4, item.data(), 10, 0);
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
    EXPECT_EQ(scope, 4);
    const char* name = id == 100   ? "bull_heal"
                       : id == 200 ? "upgrade_old"
                       : id == 201 ? "upgrade_new"
                                   : nullptr;
    if (!name) return nullptr;
    std::memcpy(current->definition.data() + 8, &scope, 4);
    std::memcpy(current->definition.data() + 16, &name, sizeof(name));
    return current->definition.data();
  }
  static modlock::gameinterop::ItemFunctions Functions() {
    return {+[](void* pawn, const char* name, uint64_t upgrade, void* extra) -> void* {
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
              auto& f = *current;
              EXPECT_EQ(component, f.pawn.data() + 0x48);
              EXPECT_EQ(item, f.item.data());
              EXPECT_EQ(flag, 0);
              ++f.removes;
              if (f.apply) f.SetCount(1);
              if (f.disconnect_on_remove) ++f.connection.generation;
            },
            +[](void* item, uint32_t bits) {
              auto& f = *current;
              ++f.writes;
              if (f.apply) std::memcpy(static_cast<unsigned char*>(item) + 0x3e, &bits, 2);
            }};
  }
};
ItemFixture* ItemFixture::current = nullptr;

TEST(PawnObserver, GrantsAnItemOnceAndPreservesTheExistingLoadout) {
  ItemFixture fixture;
  fixture.connection.image.SetIdentity(5, fixture.granted_item.data(), 11, 0);
  PawnObserver observer(fixture.Seams());
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  auto functions = ItemFixture::Functions();
  functions.add = +[](void* pawn, const char* name, uint64_t upgrade, void* extra) -> void* {
    auto& fixture = *ItemFixture::current;
    EXPECT_EQ(pawn, fixture.pawn.data());
    EXPECT_STREQ(name, "upgrade_new");
    EXPECT_EQ(upgrade, 0u);
    EXPECT_EQ(extra, nullptr);
    ++fixture.adds;
    fixture.InitAbility(fixture.granted_item, 201, 4);
    fixture.handles[2] = HandleOf(5, 11);
    fixture.SetCount(3);
    return fixture.granted_item.data();
  };
  EXPECT_FALSE(observer.GrantItem(0, 999, definitions, functions));
  EXPECT_FALSE(observer.GrantItem(0, 100, definitions, functions));
  EXPECT_EQ(fixture.adds, 0);
  const auto item = observer.GrantItem(0, 201, definitions, functions);
  ASSERT_TRUE(item) << item.error();
  EXPECT_EQ(item->slot, 4);
  EXPECT_EQ(item->handle, HandleOf(5, 11));
  ASSERT_TRUE(observer.GrantItem(0, 201, definitions, functions));
  EXPECT_EQ(fixture.adds, 1);
  EXPECT_EQ(fixture.removes, 0);
  const auto owned = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(owned);
  ASSERT_EQ(owned->size(), 3);
  EXPECT_EQ(owned->at(0).subclass_id, 100);
  EXPECT_EQ(owned->at(1).subclass_id, 200);
}

TEST(PawnObserver, ReconcilesItemsIdempotentlyWithoutRemovingHeroAbilities) {
  ItemFixture fixture;
  PawnObserver observer(fixture.Seams());
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  const std::array targets{modlock::gameinterop::ItemTarget{201, 0x10001}};
  const auto functions = ItemFixture::Functions();
  const std::array invalid{targets[0], modlock::gameinterop::ItemTarget{999, 0x10001}};
  EXPECT_FALSE(observer.ReconcileItems(0, invalid, definitions, functions));
  EXPECT_EQ(fixture.removes, 0);
  EXPECT_EQ(fixture.adds, 0);
  ASSERT_TRUE(observer.ReconcileItems(0, targets, definitions, functions));
  EXPECT_EQ(fixture.removes, 1);
  EXPECT_EQ(fixture.adds, 1);
  ASSERT_TRUE(observer.ReconcileItems(0, targets, definitions, functions));
  EXPECT_EQ(fixture.removes, 1);
  EXPECT_EQ(fixture.adds, 1);
  EXPECT_EQ(fixture.writes, 0);
  auto owned = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(owned);
  ASSERT_EQ(owned->size(), 2u);
  EXPECT_EQ((*owned)[0].subclass_id, 100u);
}

TEST(PawnObserver, RejectsFalseItemDispatchAndLostConnection) {
  ItemFixture fixture;
  PawnObserver observer(fixture.Seams());
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  const std::array targets{modlock::gameinterop::ItemTarget{201, 0x10001}};
  fixture.apply = false;
  EXPECT_FALSE(observer.ReconcileItems(0, targets, definitions, ItemFixture::Functions()));
  EXPECT_EQ(fixture.removes, 1);
  EXPECT_EQ(fixture.adds, 0);
  fixture.SetCount(1);
  EXPECT_FALSE(observer.ReconcileItems(0, targets, definitions, ItemFixture::Functions()));
  EXPECT_EQ(fixture.adds, 1);  // A non-null returned pointer cannot certify a grant.
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
  ItemFixture fixture;
  fixture.SetCount(0);
  PawnObserver observer{fixture.Seams()};
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  const std::array targets{modlock::gameinterop::AbilityUpgrade{100, 8, 65537}};
  const auto create = +[](void* component, void* definition, uint16_t slot, uint64_t upgrade,
                          bool flag, void* extra) -> void* {
    auto& fixture = *ItemFixture::current;
    EXPECT_EQ(component, fixture.pawn.data() + 0x48);
    EXPECT_EQ(definition, fixture.definition.data());
    EXPECT_EQ(slot, 8);
    EXPECT_EQ(upgrade, 0u);
    EXPECT_TRUE(flag);
    EXPECT_EQ(extra, nullptr);
    ++fixture.adds;
    if (fixture.apply) {
      fixture.InitAbility(fixture.hero, 100, slot);
      fixture.SetCount(1);
    }
    return fixture.hero.data();
  };
  const auto set_bits = ItemFixture::Functions().set_bits;
  fixture.apply = false;
  EXPECT_FALSE(observer.ReconcileAbilities(0, targets, definitions, create, set_bits));
  fixture.apply = true;
  auto restored = observer.ReconcileAbilities(0, targets, definitions, create, set_bits);
  ASSERT_TRUE(restored) << restored.error();
  const auto abilities = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(abilities);
  ASSERT_EQ(abilities->size(), 1u);
  EXPECT_EQ(abilities->front().slot, 8);
  EXPECT_EQ(abilities->front().upgrade_info, 65537u);
  EXPECT_EQ(fixture.adds, 2);
  EXPECT_TRUE(observer.ReconcileAbilities(0, targets, definitions, create, set_bits));
  EXPECT_EQ(fixture.adds, 2);
  const std::array conflict{modlock::gameinterop::AbilityUpgrade{201, 8, 65537}};
  EXPECT_FALSE(observer.ReconcileAbilities(0, conflict, definitions, create, set_bits));
  EXPECT_EQ(fixture.adds, 2);
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
  ItemFixture fixture;
  PawnObserver observer{StaminaSeams(fixture)};
  fixture.connection.occupied = true;
  WriteStamina(fixture.pawn, 40.5f, 100.0f, 12.0f, 40.5f);
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
  WriteStamina(fixture.pawn, 40.5f, std::numeric_limits<float>::infinity(), 0, 0);
  sample = observer.Observe();
  ASSERT_TRUE(sample);
  EXPECT_FALSE(sample->stamina);
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
  ItemFixture fixture;
  auto seams = StaminaSeams(fixture);
  std::array<void*, 29> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) {
    auto& f = *ItemFixture::current;
    const uint32_t replacement = HandleOf(9, 3);
    f.connection.controller = MakeController(replacement);
    f.connection.image.SetIdentity(9, f.pawn.data(), 3, 0);
    std::memcpy(f.connection.controller.data() + kPawnHandleField, &replacement,
                sizeof(replacement));
  });
  WritePointer(fixture.pawn.data(), table.data());
  PawnObserver observer{seams};
  fixture.connection.occupied = true;
  WriteStamina(fixture.pawn, 10.0f, 100.0f, 3.25f, 10.0f);
  ASSERT_TRUE(observer.Observe());
  // The notification swaps the pawn; the write must not report success.
  EXPECT_FALSE(observer.SetPracticeStamina(0, 500.0f));
}

TEST(PawnObserver, RestoresOwnedTimersWithReplicationAndReadback) {
  ItemFixture fixture;
  std::array<void*, 29> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) { ++ItemFixture::current->writes; });
  WritePointer(fixture.hero.data(), table.data());
  WritePointer(fixture.item.data(), table.data());
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
  auto invalid = targets;
  invalid.back().handle += 1;
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, invalid));
  EXPECT_EQ(fixture.writes, 0);
  invalid = targets;
  invalid.back().cooldown_end = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, invalid));
  EXPECT_EQ(fixture.writes, 0);
  invalid = {targets.front(), targets.front()};
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, invalid));
  EXPECT_EQ(fixture.writes, 0);
  ASSERT_TRUE(observer.ApplyAbilityTimers(0, targets));
  EXPECT_EQ(fixture.writes, 2);
  ASSERT_TRUE(observer.ApplyAbilityTimers(0, targets));
  EXPECT_EQ(fixture.writes, 2);
  owned = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned->front().charges, 2);
  EXPECT_EQ(owned->front().cooldown_start, 100);
  EXPECT_EQ(owned->back().charge_recharge_end, 115);
  EXPECT_EQ(owned->back().upgrade_info, 1u);

  table[28] = reinterpret_cast<void*>(+[](void* entity, const void*) {
    ++ItemFixture::current->writes;
    const float overwritten = 0;
    std::memcpy(static_cast<unsigned char*>(entity) + 0x44, &overwritten, sizeof(overwritten));
  });
  targets.front().cooldown_end = 130;
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, targets));
  EXPECT_EQ(fixture.writes, 3);
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) {
    ++ItemFixture::current->writes;
    ItemFixture::current->connection.occupied = false;
  });
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, targets));
  EXPECT_EQ(fixture.writes, 4);

  fixture.connection.occupied = true;
  ASSERT_TRUE(observer.Observe());
  table[28] = reinterpret_cast<void*>(+[](void* entity, const void*) {
    if (entity != ItemFixture::current->item.data()) return;
    const float overwritten = 0;
    std::memcpy(ItemFixture::current->hero.data() + 0x44, &overwritten, sizeof(overwritten));
  });
  targets.front().cooldown_end = 140;
  targets.back().cooldown_end = 150;
  EXPECT_FALSE(observer.ApplyAbilityTimers(0, targets));
}

TEST(PawnObserver, ReconcilesActiveItemSlotsThroughEngineSwap) {
  ItemFixture fixture;
  fixture.InitAbility(fixture.hero, 201, 7);
  fixture.InitAbility(fixture.item, 200, 4);
  PawnObserver observer{fixture.Seams()};
  ASSERT_TRUE(observer.Observe());
  modlock::gameinterop::AbilityDefinitions definitions(ItemFixture::Lookup);
  auto functions = ItemFixture::Functions();
  functions.swap_slots = +[](void* component, uint16_t first, uint16_t second) {
    auto& f = *ItemFixture::current;
    EXPECT_EQ(component, f.pawn.data() + 0x48);
    ++f.writes;
    if (!f.apply) return;
    for (auto* item : {&f.hero, &f.item}) {
      uint16_t slot;
      std::memcpy(&slot, item->data() + 0x38, sizeof(slot));
      if (slot == first)
        slot = second;
      else if (slot == second)
        slot = first;
      std::memcpy(item->data() + 0x38, &slot, sizeof(slot));
    }
  };
  const std::array targets{modlock::gameinterop::ItemTarget{200, 1, 7},
                           modlock::gameinterop::ItemTarget{201, 1, 4}};
  auto duplicate = targets;
  duplicate.back().slot = 7;
  EXPECT_FALSE(observer.ReconcileItems(0, duplicate, definitions, functions));
  EXPECT_EQ(fixture.writes, 0);
  ASSERT_TRUE(observer.ReconcileItems(0, targets, definitions, functions));
  EXPECT_EQ(fixture.writes, 1);
  ASSERT_TRUE(observer.ReconcileItems(0, targets, definitions, functions));
  EXPECT_EQ(fixture.writes, 1);
  const auto owned = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned->front().slot, 4);
  EXPECT_EQ(owned->back().slot, 7);
  auto reversed = targets;
  reversed.front().slot = 4;
  reversed.back().slot = 7;
  fixture.apply = false;
  EXPECT_FALSE(observer.ReconcileItems(0, reversed, definitions, functions));
  EXPECT_EQ(fixture.writes, 2);
}

TEST(PawnObserver, FreshAbilityTargetsFullChargesAndClearedTimers) {
  ItemFixture fixture;
  auto seams = fixture.Seams();
  seams.max_charges = [](void*) -> std::expected<int32_t, std::string> { return 5; };
  PawnObserver observer{seams};
  ASSERT_TRUE(observer.Observe());

  // Seed reduced charges and live timers on both owned abilities.
  const int32_t reduced_charges = 2;
  std::memcpy(fixture.hero.data() + 0x40, &reduced_charges, sizeof(reduced_charges));
  std::memcpy(fixture.item.data() + 0x40, &reduced_charges, sizeof(reduced_charges));
  const float cooldown = 30.0f, recharge = 12.5f;
  std::memcpy(fixture.hero.data() + 0x44, &cooldown, sizeof(cooldown));
  std::memcpy(fixture.item.data() + 0x44, &recharge, sizeof(recharge));

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
  const auto unchanged = observer.CurrentAbilitiesForSlot(0);
  ASSERT_TRUE(unchanged);
  for (const auto& ability : *unchanged) EXPECT_EQ(ability.charges, reduced_charges);
}

TEST(PawnObserver, FreshAbilityTargetsHonorsExplicitZeroAndRejectsReadFailure) {
  ItemFixture fixture;
  auto seams = fixture.Seams();
  // Native chargeless ability: the seam reports an explicit 0, not absence.
  seams.max_charges = [](void*) -> std::expected<int32_t, std::string> { return 0; };
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
  static Fixture* active = nullptr;
  static int observer_spawns = 0;
  Fixture fixture;
  fixture.occupied = true;
  fixture.generation = 7;
  active = &fixture;
  observer_spawns = 0;
  std::array<void*, 106> table{};
  table[105] = reinterpret_cast<void*>(+[](void*, int) { ++active->generation; });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.spawn_observer = +[](void*) -> void* {
    ++observer_spawns;
    return nullptr;
  };
  PawnObserver observer{SeamsFor(fixture)};
  const auto refused = observer.SelectPlayer(0, fixture.xuid, 7, 1, nullptr, calls);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().find("connection changed"), std::string::npos);
  EXPECT_EQ(observer_spawns, 0);
  active = nullptr;
}

TEST(PawnObserver, SpectatorSelectionNeedsNoHeroAndDoesNotCreateCombatPawn) {
  static int observer_spawns = 0;
  static Fixture* active = nullptr;
  observer_spawns = 0;
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 1;
  std::array<void*, 106> table{};
  table[105] =
      reinterpret_cast<void*>(+[](void*, int) { ADD_FAILURE() << "already on spectator team"; });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.spawn_observer = +[](void*) -> void* {
    ++observer_spawns;
    // The engine can return an actor before publishing m_hPawn.
    return active->controller.data();
  };
  PawnObserver observer{SeamsFor(fixture)};
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
  EXPECT_FALSE(observer.SelectPlayer(0, fixture.xuid + 1, 3, 1, nullptr, calls));
  EXPECT_EQ(observer_spawns, 1);
}

TEST(PawnObserver, HeroSelectionDoesNotTreatObserverPawnAsHeroPawn) {
  static Fixture* active = nullptr;
  static void* hero_pawn = nullptr;
  static int created = 0;
  static int selected = 0;
  constexpr size_t hero_handle_field = 0x48;
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 2;
  std::array<void*, 106> table{};
  table[105] = reinterpret_cast<void*>(+[](void*, int) { ADD_FAILURE(); });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  auto spectator = MakeController(0);
  auto hero = MakeController(0);
  hero_pawn = hero.data();
  fixture.image.SetIdentity(2, spectator.data(), 7, 0);
  fixture.image.SetIdentity(3, hero.data(), 8, 0);
  const auto spectator_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kPawnHandleField, &spectator_handle,
              sizeof(spectator_handle));
  auto seams = SeamsFor(fixture);
  const auto old_layout = seams.layout;
  seams.layout = [old_layout, hero_handle_field] {
    auto layout = old_layout();
    layout->hero_pawn_handle = hero_handle_field;
    return layout;
  };
  modlock::gameinterop::PlayerSelectionCalls calls;
  created = selected = 0;
  calls.create_pawn = +[](void*, int) -> void* {
    ++created;
    const auto handle = HandleOf(3, 8);
    std::memcpy(active->controller.data() + hero_handle_field, &handle, sizeof(handle));
    return hero_pawn;
  };
  calls.select_hero = +[](void* pawn, void*) {
    EXPECT_EQ(pawn, hero_pawn);
    ++selected;
  };
  PawnObserver observer{seams};
  EXPECT_TRUE(observer.SelectPlayer(0, fixture.xuid, 3, 2, &fixture, calls));
  EXPECT_EQ(created, 1);
  EXPECT_EQ(selected, 1);
}

TEST(PawnObserver, RespawnUsesDeadHeroHandleAndRejectsStaleConnections) {
  static void* expected_pawn = nullptr;
  static int respawned = 0;
  constexpr size_t hero_handle_field = 0x48;
  Fixture fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance spectator{}, hero{};
  expected_pawn = hero.data();
  respawned = 0;
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, spectator.data(), 7, 0);
  fixture.image.SetIdentity(3, hero.data(), 8, 0);
  auto hero_handle = HandleOf(3, 8);
  std::memcpy(fixture.controller.data() + hero_handle_field, &hero_handle, sizeof(hero_handle));
  auto seams = SeamsFor(fixture);
  seams.layout = [hero_handle_field]() -> std::expected<EntityLayout, std::string> {
    auto layout = FixtureLayout();
    layout.hero_pawn_handle = hero_handle_field;
    return layout;
  };
  PawnObserver observer{seams};
  auto respawn = +[](void* pawn, bool force) {
    EXPECT_EQ(pawn, expected_pawn);
    EXPECT_TRUE(force);
    ++respawned;
    const int32_t health = 100;
    std::memcpy(static_cast<char*>(pawn) + kHealthField, &health, sizeof(health));
  };
  EXPECT_FALSE(observer.RespawnPlayer(0, 2, respawn));
  EXPECT_EQ(respawned, 0);
  ASSERT_TRUE(observer.RespawnPlayer(0, 3, respawn));
  EXPECT_EQ(respawned, 1);
  ASSERT_TRUE(observer.RespawnPlayer(0, 3, respawn));
  EXPECT_EQ(respawned, 1);
  hero_handle = HandleOf(3, 9);
  std::memcpy(fixture.controller.data() + hero_handle_field, &hero_handle, sizeof(hero_handle));
  EXPECT_FALSE(observer.RespawnPlayer(0, 3, respawn));
  EXPECT_EQ(respawned, 1);
}

TEST(PawnObserver, DirectMovementFieldsAreFiniteAndGroundProvenanceIsExplicit) {
  Fixture fixture;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance pawn{}, body{}, scene{}, modifiers{};
  WritePointer(pawn.data() + 0xa0, modifiers.data());
  const float origin[3] = {1, 2, 3};
  const float velocity[3] = {100, -2, 0.5f};
  const uint32_t ground = 0xffffffff;
  WritePointer(body.data() + kSceneNodeField, scene.data());
  std::memcpy(scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  std::memcpy(pawn.data() + kAbsVelocityField, velocity, sizeof(velocity));
  std::memcpy(pawn.data() + kGroundEntityField, &ground, sizeof(ground));
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
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  ASSERT_TRUE(sample->movement.has_value());
  ASSERT_TRUE(sample->movement->abs_velocity.has_value());
  EXPECT_FLOAT_EQ((*sample->movement->abs_velocity)[0], 100);
  ASSERT_TRUE(sample->movement->ground_entity_handle.has_value());
  ASSERT_TRUE(sample->movement->grounded_by_handle.has_value());
  EXPECT_EQ(*sample->movement->ground_entity_handle, 0xffffffffu);
  EXPECT_FALSE(*sample->movement->grounded_by_handle);

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
  Fixture fixture;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance pawn{}, body{}, scene{};
  const float origin[3] = {1, 2, 3};
  const float velocity[3] = {100, std::numeric_limits<float>::quiet_NaN(), 0.5f};
  WritePointer(body.data() + kSceneNodeField, scene.data());
  std::memcpy(scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  std::memcpy(pawn.data() + kAbsVelocityField, velocity, sizeof(velocity));
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);
  fixture.occupied = true;
  PawnObserver::Seams seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    return MovementFixtureLayout();
  };
  PawnObserver observer{std::move(seams), true};
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  ASSERT_TRUE(sample->movement.has_value());
  EXPECT_FALSE(sample->movement->abs_velocity.has_value());
}

TEST(PawnObserver, MissingMovementFieldsRemainAbsentWithoutHidingIdentity) {
  Fixture fixture;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance pawn{}, body{}, scene{};
  const float origin[3] = {1, 2, 3};
  WritePointer(body.data() + kSceneNodeField, scene.data());
  std::memcpy(scene.data() + kAbsOriginField, origin, sizeof(origin));
  WritePointer(pawn.data() + kBodyComponentField, body.data());
  fixture.image.SetIdentity(1, fixture.controller.data(), 5, 0);
  fixture.image.SetIdentity(2, pawn.data(), 7, 0);
  fixture.occupied = true;
  PawnObserver::Seams seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    return MovementFixtureLayout(false, false);
  };
  PawnObserver observer{std::move(seams), true};
  auto sample = observer.Observe();
  ASSERT_TRUE(sample.has_value());
  EXPECT_EQ(sample->steam_id, fixture.xuid);
  ASSERT_TRUE(sample->movement.has_value());
  EXPECT_FALSE(sample->movement->abs_velocity.has_value());
  EXPECT_FALSE(sample->movement->ground_entity_handle.has_value());
  EXPECT_FALSE(sample->movement->grounded_by_handle.has_value());
}
