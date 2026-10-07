#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>

#include "modlock/gameinterop/world_entities.h"

namespace {

using modlock::gameinterop::MakeMemberName;
using modlock::gameinterop::WorldEntities;

// NativeWorld keeps the native class, identity and subclass layouts consumed
// by CreateEntity and Spawn.
class NativeWorld : public testing::Test {
 protected:
  // entity_class reaches schema through CEntityClass::m_pClassInfo and
  // CEntityClassInfo::m_pSchemaBinding; schema binds itself and holds its
  // class name second.
  static inline std::array<void*, 12> entity_class;
  static inline std::array<void*, 6> class_info;
  static inline std::array<void*, 8> schema;
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
    entity_class.fill(nullptr);
    class_info.fill(nullptr);
    schema.fill(nullptr);
    entity_class[11] = class_info.data();
    class_info[5] = schema.data();
    schema[0] = schema.data();
    schema[1] = const_cast<char*>("CNPC_Trooper");
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
    calls.find_class = [](void* world, const char*, void*) -> void* {
      EXPECT_EQ(world, system.data());
      return entity_class.data();
    };
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

TEST_F(NativeWorld, CreateEntityInstallsItsSubclassBeforeNativeSpawn) {
  schema[1] = const_cast<char*>("CCitadelItemPickupIdol");
  auto world = World();
  WorldEntities::Target target{.designer_name = "citadel_item_pickup_idol",
                               .subclass_id = WorldEntities::SubclassId("citadel_item_pickup_idol"),
                               .position = {-6579, 0, 144}};
  const auto pickup = world.CreateEntity(target, {});
  ASSERT_TRUE(pickup) << pickup.error();
  EXPECT_EQ(*pickup, kHandle);
  EXPECT_EQ(designer, "citadel_item_pickup_idol");
  EXPECT_EQ(subclass, MakeMemberName("citadel_item_pickup_idol").hash);
  EXPECT_EQ(position, (std::array<float, 3>{-6579, 0, 144}));
  EXPECT_EQ(created, 1);
  EXPECT_EQ(spawned, 1);
}

TEST_F(NativeWorld, MissingDefinitionNeverReachesNativeSpawn) {
  has_definition = false;
  auto world = World();
  WorldEntities::Target target{
      .designer_name = "citadel_item_pickup_idol",
      .subclass_id = WorldEntities::SubclassId("citadel_item_pickup_idol")};
  const auto pickup = world.CreateEntity(target, {});
  ASSERT_FALSE(pickup);
  EXPECT_EQ(pickup.error(), "entity subclass is absent from this game build");
  EXPECT_EQ(created, 0);
  EXPECT_EQ(spawned, 0);
}

TEST_F(NativeWorld, AnAbilityIsRefusedBeforeItExists) {
  schema[1] = const_cast<char*>("CCitadelBaseAbility");
  auto world = World();
  const auto ability = world.CreateEntity({.designer_name = "upgrade_spellshield"}, {});
  ASSERT_FALSE(ability);
  EXPECT_EQ(ability.error(),
            "upgrade_spellshield is an ability, which only a hero holds: give it with GiveItem or "
            "ReplaceAbility");
  EXPECT_EQ(created, 0);
}

TEST_F(NativeWorld, AnEntityThatCrashesAloneIsRefusedBeforeItExists) {
  auto world = World();
  const auto sentry = world.CreateEntity({.designer_name = "npc_shielded_sentry"}, {});
  ASSERT_FALSE(sentry);
  EXPECT_EQ(sentry.error(),
            "npc_shielded_sentry crashes the server when created alone: it needs the hero who "
            "owns it");
  EXPECT_EQ(created, 0);
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
