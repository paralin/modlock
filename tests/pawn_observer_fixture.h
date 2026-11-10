#pragma once

// The recorded entity images, layouts and engine callbacks the pawn observer
// tests share. They model the pinned sourcesdk layouts and are independent of
// the production constants on purpose.

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "modlock/gameinterop/pawn_observer.h"

namespace pawn_observer_test {

using modlock::gameinterop::EntityLayout;
using modlock::gameinterop::MovementLayout;
using modlock::gameinterop::PawnObserver;

// Pinned CEntityIdentity layout (sourcesdk public/entity2/entityidentity.h,
// "Size: 0x70").
inline constexpr size_t kIdentityStride = 0x70;
inline constexpr size_t kIdentityInstanceOffset = 0x00;
inline constexpr size_t kIdentityEHandleOffset = 0x10;
inline constexpr size_t kIdentityFlagsOffset = 0x30;
inline constexpr size_t kIdentityBackPointer = 0x10;  // CEntityInstance::m_pEntity
inline constexpr uint32_t kMaxEntitiesInList = 512;   // MAX_ENTITIES_IN_LIST
inline constexpr int kMaxEntityLists = 64;            // MAX_ENTITY_LISTS

// Fake field offsets inside the fixture entity instances. Independent of any
// production constant on purpose: a drift between them fails resolution.
inline constexpr size_t kPawnHandleField = 0x40;
inline constexpr size_t kBodyComponentField = 0x20;
inline constexpr size_t kSceneNodeField = 0x08;
inline constexpr size_t kAbsOriginField = 0x40;
inline constexpr size_t kAbsVelocityField = 0x80;
inline constexpr size_t kGroundEntityField = 0x90;
inline constexpr size_t kHeroField = 0x50;
inline constexpr size_t kTeamField = 0x54;
inline constexpr size_t kHealthField = 0x58;
inline constexpr size_t kHeroHandleField = 0x48;
inline constexpr size_t kPlayStateField = 0x60;
// Fake controller-side scoreboard: PlayerDataGlobal_t inline in the
// controller, then its int32 counters in ScoreTotals order. Independent of
// any production constant on purpose.
inline constexpr size_t kPlayerDataGlobalField = 0x80;
// Fake controller-side m_nHeroID and m_nPreGameHeroID, past the counters.
inline constexpr size_t kPlayerHeroIdField = 0xb0;
inline constexpr size_t kPregameHeroIdField = 0xb4;

inline uint32_t HandleOf(uint32_t index, uint32_t serial) {
  return (index & 0x7FFF) | ((serial & 0x1FFFF) << 15);
}

inline void WritePointer(void* at, void* value) {
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

inline Instance MakeController(uint32_t pawn_handle) {
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

inline modlock::gameinterop::ScoreLayout FixtureScore() {
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

inline EntityLayout FixtureLayout() {
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

inline EntityLayout MovementFixtureLayout(bool velocity = true, bool ground = true) {
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

inline PawnObserver::Seams SeamsFor(Fixture& fixture) {
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
inline PawnObserver::Seams HeroSeamsFor(Fixture& fixture) {
  // Extend the entity layout with selection's controller fields.
  auto seams = SeamsFor(fixture);
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    // Provide the controller fields that selection reads and writes.
    auto layout = FixtureLayout();
    layout.hero_pawn_handle = kHeroHandleField;
    layout.player_hero_id = kPlayerHeroIdField;
    layout.pregame_hero_id = kPregameHeroIdField;
    layout.play_state = kPlayStateField;
    return layout;
  };
  return seams;
}

// Engine callback fixture over the same entity/handle memory used by the observer.
struct ItemFixture {
  Fixture connection;
  Instance pawn{}, body{}, scene{}, hero{}, item{}, granted_item{};
  std::array<uint32_t, 4> handles{};
  std::array<unsigned char, 0x30> definition{};
  int adds = 0, removes = 0, writes = 0, trains = 0, buys = 0, imbues = 0;
  bool accept = true;
  bool apply = true;
  bool disconnect_on_remove = false;
  static inline ItemFixture* current = nullptr;

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
                // Give the item its ability.
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

// Stamina fixture offsets: the resource floats sit behind the inline ability
// component. Independent of any production constant on purpose. The pawn
// instance has room for 0xa0 bytes; the floats occupy its tail.
inline constexpr size_t kStaminaCurrentField = 0x80;
inline constexpr size_t kStaminaMaxField = 0x84;
inline constexpr size_t kStaminaLatchTimeField = 0x88;
inline constexpr size_t kStaminaLatchValueField = 0x8c;
inline constexpr size_t kStaminaPrevRegenField = 0x90;

inline modlock::gameinterop::StaminaLayout StaminaFixtureLayout() {
  return {.current = kStaminaCurrentField,
          .max = kStaminaMaxField,
          .latch_time = kStaminaLatchTimeField,
          .latch_value = kStaminaLatchValueField};
}

// The observer reads and restores stamina through the resolved EntityLayout;
// the seam composes the fixture offsets with the shared fixture layout.
inline PawnObserver::Seams StaminaSeams(ItemFixture& fixture) {
  auto seams = fixture.Seams();
  seams.layout = []() -> std::expected<EntityLayout, std::string> {
    // Provide the controller fields that selection reads and writes.
    auto layout = FixtureLayout();
    layout.stamina = StaminaFixtureLayout();
    return layout;
  };
  return seams;
}

// NotifyFullState requires the pinned vtable slot 27 on the notified entity.
// The lambda cannot capture; it reaches the fixture through the static
// current pointer the same way the item reconciliation tests do.
inline void AttachNotifyTable(ItemFixture& fixture) {
  static std::array<void*, 29> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) { ++ItemFixture::current->writes; });
  WritePointer(fixture.pawn.data(), table.data());
}

inline void WriteStamina(Instance& pawn, float current, float max, float latch_time,
                         float latch_value) {
  std::memcpy(pawn.data() + kStaminaCurrentField, &current, sizeof(current));
  std::memcpy(pawn.data() + kStaminaMaxField, &max, sizeof(max));
  std::memcpy(pawn.data() + kStaminaLatchTimeField, &latch_time, sizeof(latch_time));
  std::memcpy(pawn.data() + kStaminaLatchValueField, &latch_value, sizeof(latch_value));
}

}  // namespace pawn_observer_test
