#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <variant>

#include "modlock/gameinterop/world_entities.h"

namespace {

using modlock::gameinterop::EntityKeyValue;
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
  static inline std::map<std::string, std::variant<bool, int, float, std::string>> key_values;
  static constexpr size_t kSubclassOffset = 0x314;
  static constexpr uint32_t kHandle = 0x18000;
  static constexpr size_t kPreparedOffset = 0x200;

  template <typename T>
  static void Write(std::byte* at, const T& value) {
    std::memcpy(at, &value, sizeof(value));
  }

  void SetUp() override {
    // Clear the entity and its class chain, naming it a trooper.
    entity.fill({});
    entity_class.fill(nullptr);
    class_info.fill(nullptr);
    schema.fill(nullptr);
    entity_class[11] = class_info.data();
    class_info[5] = schema.data();
    schema[0] = schema.data();
    schema[1] = const_cast<char*>("CNPC_Trooper");

    // Clear the identity, system and vtable.
    identity.fill({});
    system.fill(nullptr);
    vtable.fill(nullptr);
    created = spawned = removed = 0;

    // Clear what the last test recorded.
    prepared = 0;
    has_definition = true;
    subclass = 0;
    designer.clear();
    position = {};
    key_values.clear();

    // Link the identity, entity and vtable, with Teleport in slot 163.
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
    // Find the system and the entity class.
    WorldEntities::Calls calls;
    calls.entity_system = []() -> std::expected<void*, std::string> {
      return system.data();
    };
    calls.find_class = [](void* world, const char*, void*) -> void* {
      EXPECT_EQ(world, system.data());
      return entity_class.data();
    };

    // Resolve definitions, recording the subclass token.
    calls.offsets = {0x30, kSubclassOffset, 0, 0, 0, 0, 0};
    calls.definition = [](int32_t type, uint32_t token) -> void* {
      EXPECT_EQ(type, -1);
      subclass = token;
      return has_definition ? definition.data() : nullptr;
    };

    // Record creation and each keyvalue.
    calls.create = [](void*, const char* name, int) -> void* {
      ++created;
      designer = name;
      return entity.data();
    };
    calls.key_values.create_key_values = []() -> void* {
      return definition.data();
    };
    calls.key_values.set_string = [](void*, const modlock::gameinterop::MemberName* name,
                                     const char* value) {
      key_values[name->string] = std::string(value);
    };
    calls.key_values.set_bool = [](void*, const modlock::gameinterop::MemberName* name,
                                   unsigned char value) {
      key_values[name->string] = value != 0;
    };
    calls.key_values.set_int = [](void*, const modlock::gameinterop::MemberName* name, int value) {
      key_values[name->string] = value;
    };
    calls.key_values.set_float = [](void*, const modlock::gameinterop::MemberName* name,
                                    float value) {
      key_values[name->string] = value;
    };

    // Queue checks its arguments.
    calls.queue = [](void* world, void* id, void* properties) {
      EXPECT_EQ(world, system.data());
      EXPECT_EQ(id, identity.data());
      EXPECT_NE(properties, nullptr);
    };

    // Spawn checks the subclass and records preparation.
    calls.execute = [](void*) {
      // Native urn Spawn dereferences VData + 0x118 to install its pickup aura.
      // A subclassed entity's token and resolved pointer must both exist
      // before queued creation.
      if (subclass != 0) {
        uint32_t token = 0;
        void* vdata = nullptr;
        std::memcpy(&token, entity.data() + kSubclassOffset, sizeof(token));
        std::memcpy(&vdata, entity.data() + kSubclassOffset + sizeof(token), sizeof(vdata));
        EXPECT_EQ(token, subclass);
        EXPECT_EQ(vdata, definition.data());
      }
      std::memcpy(&prepared, entity.data() + kPreparedOffset, sizeof(prepared));
      ++spawned;
    };
    calls.remove = [](void*) {
      ++removed;
    };
    return WorldEntities(calls);
  }
};

TEST_F(NativeWorld, CreateEntityInstallsItsSubclassBeforeNativeSpawn) {
  // Create a pickup with its subclass.
  schema[1] = const_cast<char*>("CCitadelItemPickupIdol");
  auto world = World();
  WorldEntities::Target target{.designer_name = "citadel_item_pickup_idol",
                               .subclass_id = WorldEntities::SubclassId("citadel_item_pickup_idol"),
                               .position = {-6579, 0, 144}};
  const auto pickup = world.CreateEntity(target, {});
  ASSERT_TRUE(pickup) << pickup.error();

  // The subclass and position were installed before one spawn.
  EXPECT_EQ(*pickup, kHandle);
  EXPECT_EQ(designer, "citadel_item_pickup_idol");
  EXPECT_EQ(subclass, MakeMemberName("citadel_item_pickup_idol").hash);
  EXPECT_EQ(position, (std::array<float, 3>{-6579, 0, 144}));
  EXPECT_EQ(created, 1);
  EXPECT_EQ(spawned, 1);
}

TEST_F(NativeWorld, MissingDefinitionNeverReachesNativeSpawn) {
  // Without a definition, nothing is created.
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

TEST_F(NativeWorld, WorldTextPanelGetsStockClientDefaults) {
  // Create a panel with only a message.
  auto world = World();
  const std::array values{
      EntityKeyValue{.key = "message", .value = std::string_view("TEXT PANEL")},
  };
  const auto panel =
      world.CreateEntity({.designer_name = "point_clientui_world_text_panel"}, values);
  ASSERT_TRUE(panel) << panel.error();

  // Each unset value takes the stock default.
  EXPECT_EQ(std::get<bool>(key_values.at("enabled")), true);
  EXPECT_EQ(std::get<float>(key_values.at("width")), 128.0f);
  EXPECT_EQ(std::get<float>(key_values.at("height")), 32.0f);
  EXPECT_EQ(std::get<float>(key_values.at("panel_dpi")), 1.0f);
  EXPECT_EQ(std::get<float>(key_values.at("interact_distance")), 0.0f);
  EXPECT_EQ(std::get<bool>(key_values.at("ignore_input")), true);
  EXPECT_EQ(std::get<std::string>(key_values.at("dialog_layout_name")),
            "file://{resources}/layout/hideout/hideout_in_world_text_label.xml");
  EXPECT_EQ(std::get<int>(key_values.at("owner_context")), 0);

  // It aligns and turns like the stock label, and carries the message.
  EXPECT_EQ(std::get<int>(key_values.at("horizontal_align")), 1);
  EXPECT_EQ(std::get<int>(key_values.at("vertical_align")), 0);
  EXPECT_EQ(std::get<int>(key_values.at("orientation")), 1);
  EXPECT_EQ(std::get<bool>(key_values.at("ignore_parent_orientation")), true);
  EXPECT_EQ(std::get<std::string>(key_values.at("message")), "TEXT PANEL");
}

TEST_F(NativeWorld, WorldTextPanelKeepsExplicitSpawnValues) {
  // Values given at spawn are kept.
  auto world = World();
  const std::array values{
      EntityKeyValue{.key = "message", .value = std::string_view("CUSTOM TEXT")},
      EntityKeyValue{.key = "width", .value = 240.0f},
      EntityKeyValue{.key = "dialog_layout_name",
                     .value = std::string_view("file://{resources}/layout/custom.xml")},
  };
  const auto panel =
      world.CreateEntity({.designer_name = "point_clientui_world_text_panel"}, values);
  ASSERT_TRUE(panel) << panel.error();
  EXPECT_EQ(std::get<float>(key_values.at("width")), 240.0f);
  EXPECT_EQ(std::get<std::string>(key_values.at("dialog_layout_name")),
            "file://{resources}/layout/custom.xml");
  EXPECT_EQ(std::get<std::string>(key_values.at("message")), "CUSTOM TEXT");
}

TEST_F(NativeWorld, AnAbilityIsRefusedBeforeItExists) {
  // An ability is refused before anything is created.
  schema[1] = const_cast<char*>("CCitadelBaseAbility");
  auto world = World();
  const auto ability = world.CreateEntity({.designer_name = "upgrade_spellshield"}, {});
  ASSERT_FALSE(ability);
  EXPECT_EQ(ability.error(),
            "upgrade_spellshield is an ability, which only a hero holds: give it with GiveItem or "
            "ReplaceAbility");
  EXPECT_EQ(created, 0);
}

TEST_F(NativeWorld, ATextValueTooLongToNetworkIsRefusedBeforeItExists) {
  // Text one character past the network limit is refused.
  auto world = World();
  const std::string limit(WorldEntities::kMaxKeyValueText, 'a');
  const std::string over(WorldEntities::kMaxKeyValueText + 1, 'a');
  const EntityKeyValue too_long[] = {{"panel_id", std::string_view(over)}};
  const auto dialog = world.CreateEntity({.designer_name = "point_clientui_dialog"}, too_long);
  ASSERT_FALSE(dialog);
  EXPECT_EQ(dialog.error(),
            "key value panel_id is 65501 characters; at most 65500 can be networked");
  EXPECT_EQ(created, 0);

  // Text at the limit is created.
  const EntityKeyValue at_limit[] = {{"panel_id", std::string_view(limit)}};
  ASSERT_TRUE(world.CreateEntity({.designer_name = "point_clientui_dialog"}, at_limit));
  EXPECT_EQ(created, 1);
}

TEST_F(NativeWorld, AnEntityThatCrashesAloneIsRefusedBeforeItExists) {
  // An entity that crashes alone is refused.
  auto world = World();
  const auto sentry = world.CreateEntity({.designer_name = "npc_shielded_sentry"}, {});
  ASSERT_FALSE(sentry);
  EXPECT_EQ(sentry.error(),
            "npc_shielded_sentry crashes the server when created alone: it needs the hero who "
            "owns it");
  EXPECT_EQ(created, 0);
}

TEST_F(NativeWorld, SpawnPreparesTheUnitBeforeNativeSpawn) {
  // Preparation runs before the unit spawns.
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
  // A failed preparation removes the unit before it spawns.
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
