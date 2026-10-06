#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>

#include "modlock/gameinterop/world_entities.h"

namespace {

using modlock::gameinterop::MakeMemberName;
using modlock::gameinterop::WorldEntities;

// NativeWorld keeps the native identity and subclass layouts consumed by
// CreatePickup and Spawn.
class NativeWorld : public testing::Test {
 protected:
  static inline std::array<std::byte, 0x400> entity;
  static inline std::array<std::byte, 0x70> identity;
  static inline std::array<void*, 66> system;
  static inline std::array<void*, 164> vtable;
  static inline std::array<std::byte, 0x140> definition;
  static inline uint32_t subclass;
  static inline bool has_definition;
  static inline int created;
  static inline int spawned;
  static inline int removed;
  static inline uint32_t prepared;
  static inline std::string designer;
  static inline std::array<float, 3> position;
  static constexpr size_t kSubclassOffset = 0x314;
  static constexpr uint32_t kHandle = 0x18000;
  static constexpr size_t kPreparedOffset = 0x200;

  template <typename T>
  static void Write(std::byte* at, const T& value) {
    std::memcpy(at, &value, sizeof(value));
  }

  void SetUp() override {
    entity.fill({});
    identity.fill({});
    system.fill(nullptr);
    vtable.fill(nullptr);
    created = spawned = removed = 0;
    prepared = 0;
    has_definition = true;
    subclass = 0;
    designer.clear();
    position = {};
    system[2] = identity.data();
    Write(identity.data(), entity.data());
    Write(identity.data() + 0x10, kHandle);
    Write(entity.data(), vtable.data());
    Write(entity.data() + 0x10, identity.data());
    vtable[163] = reinterpret_cast<void*>(&Teleport);
  }

  static void Teleport(void*, const float* origin, const float*, const float*) {
    std::memcpy(position.data(), origin, sizeof(position));
  }

  static WorldEntities World() {
    WorldEntities::Calls calls;
    calls.entity_system = []() -> std::expected<void*, std::string> { return system.data(); };
    calls.offsets = {0x30, kSubclassOffset, 0, 0, 0, 0, 0};
    calls.definition = [](int32_t type, uint32_t token) -> void* {
      EXPECT_EQ(type, -1);
      subclass = token;
      return has_definition ? definition.data() : nullptr;
    };
    calls.create = [](void*, const char* name, int) -> void* {
      ++created;
      designer = name;
      return entity.data();
    };
    calls.key_values.create_key_values = []() -> void* { return definition.data(); };
    calls.queue = [](void* world, void* id, void* properties) {
      EXPECT_EQ(world, system.data());
      EXPECT_EQ(id, identity.data());
      EXPECT_NE(properties, nullptr);
    };
    calls.execute = [](void*) {
      uint32_t token = 0;
      void* vdata = nullptr;
      std::memcpy(&token, entity.data() + kSubclassOffset, sizeof(token));
      std::memcpy(&vdata, entity.data() + kSubclassOffset + sizeof(token), sizeof(vdata));
      // Native urn Spawn dereferences VData + 0x118 to install its pickup aura.
      // Both the token and its resolved pointer must exist before queued creation.
      EXPECT_NE(token, 0u);
      EXPECT_EQ(token, subclass);
      EXPECT_EQ(vdata, definition.data());
      std::memcpy(&prepared, entity.data() + kPreparedOffset, sizeof(prepared));
      ++spawned;
    };
    calls.remove = [](void*) { ++removed; };
    return WorldEntities(calls);
  }
};

TEST_F(NativeWorld, UrnInstallsItsSubclassBeforeNativeSpawn) {
  auto world = World();
  const auto pickup = world.CreatePickup(WorldEntities::Pickup::kUrn, {-6579, 0, 144});
  ASSERT_TRUE(pickup) << pickup.error();
  EXPECT_EQ(*pickup, kHandle);
  EXPECT_EQ(designer, "citadel_item_pickup_idol");
  EXPECT_EQ(subclass, MakeMemberName("citadel_item_pickup_idol").hash);
  EXPECT_EQ(position, (std::array<float, 3>{-6579, 0, 144}));
  EXPECT_EQ(created, 1);
  EXPECT_EQ(spawned, 1);
}

TEST_F(NativeWorld, MissingUrnDefinitionNeverReachesNativeSpawn) {
  has_definition = false;
  auto world = World();
  const auto pickup = world.CreatePickup(WorldEntities::Pickup::kUrn, {0, 0, 24});
  ASSERT_FALSE(pickup);
  EXPECT_EQ(pickup.error(), "entity subclass is absent from this game build");
  EXPECT_EQ(created, 0);
  EXPECT_EQ(spawned, 0);
}

TEST_F(NativeWorld, MovementBuffKeepsItsOwnSubclass) {
  auto world = World();
  const auto pickup = world.CreatePickup(WorldEntities::Pickup::kMovementBuff, {0, 0, 16});
  ASSERT_TRUE(pickup) << pickup.error();
  EXPECT_EQ(designer, "citadel_item_pickup");
  EXPECT_EQ(subclass, MakeMemberName("movement_powerup_pickup").hash);
  EXPECT_EQ(spawned, 1);
}

TEST_F(NativeWorld, SpawnPreparesTheUnitBeforeNativeSpawn) {
  auto world = World();
  WorldEntities::Target target{.designer_name = "npc_trooper",
                               .subclass_id = WorldEntities::SubclassId("trooper_melee"),
                               .team = 2};
  const auto npc = world.Spawn(target, [](void* unit) -> std::expected<void, std::string> {
    Write(static_cast<std::byte*>(unit) + kPreparedOffset, uint32_t{3});
    return {};
  });
  ASSERT_TRUE(npc) << npc.error();
  EXPECT_EQ(prepared, 3u);
  EXPECT_EQ(spawned, 1);
}

TEST_F(NativeWorld, FailedPreparationRemovesTheUnitUnspawned) {
  auto world = World();
  WorldEntities::Target target{.designer_name = "npc_trooper",
                               .subclass_id = WorldEntities::SubclassId("trooper_melee"),
                               .team = 2};
  const auto npc = world.Spawn(target, [](void*) -> std::expected<void, std::string> {
    return std::unexpected("the entity is a CNPC_Trooper, not a CCitadelPlayerPawn");
  });
  ASSERT_FALSE(npc);
  EXPECT_EQ(npc.error(), "the entity is a CNPC_Trooper, not a CCitadelPlayerPawn");
  EXPECT_EQ(created, 1);
  EXPECT_EQ(removed, 1);
  EXPECT_EQ(spawned, 0);
}

}  // namespace
