// Contract tests for AbilityModifier on arbitrary live entities: embedded and
// standalone modifiers resolve in scope 2, replicate through m_bPredictedOwner,
// pass the parent ability subclass hash when embedded, detect attached
// instances, refuse dead entities, and restore the shared definition duration.
#include "modlock/gameinterop/ability_modifier.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "gameinterop/schema_layout.h"
#include "gameinterop/utl_vector.h"
#include "gtest/gtest.h"
#include "modlock/gameinterop/keyvalues.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace {

namespace schema = modlock::gameinterop::schema;
using modlock::gameinterop::AbilityModifier;
using modlock::gameinterop::FindVirtualSlot;
using modlock::gameinterop::kInvalidEHandleIndex;
using modlock::gameinterop::MakeMemberName;
using modlock::gameinterop::UtlVector;

schema::Field MakeField(const char* name, const schema::DeclaredType& declared, int32_t offset) {
  return schema::Field{
      .name = name, .type = reinterpret_cast<const schema::Type*>(&declared), .offset = offset};
}

struct ModifierSchema {
  struct Scope {
    void** vtable;
    ModifierSchema* owner;
  };

  struct System {
    void** vtable;
    Scope* scope;
  };

  static void* FindScope(void* self, const char* module, const char**) {
    return std::string_view(module) == "server.dll" ? static_cast<System*>(self)->scope : nullptr;
  }

  static void FindClass(void* self, void* out, const char* name) {
    auto* owner = static_cast<Scope*>(self)->owner;
    for (const auto& info : owner->classes) {
      if (std::string_view(info.name) == name) {
        *static_cast<const void**>(out) = &info;
        return;
      }
    }
    *static_cast<void**>(out) = nullptr;
  }

  ModifierSchema() {
    system_methods[FindVirtualSlot("schema-system.find-type-scope")->index] =
        reinterpret_cast<void*>(&FindScope);
    scope_methods[FindVirtualSlot("schema-type-scope.find-declared-class")->index] =
        reinterpret_cast<void*>(&FindClass);

    pointer_type.name = "CModifierProperty*";
    byte_type.name = "uint8";
    list_type.name = "CUtlVector< CBaseModifier* >";
    bool_type.name = "bool";
    duration_type.name = "CModifierLevelFloat";

    entity_fields = {
        MakeField("m_lifeState", byte_type, 8),
        MakeField("m_pModifierProp", pointer_type, 16),
    };
    classes.push_back(schema::ClassInfo{
        .name = "CBaseEntity",
        .size = 24,
        .field_count = static_cast<uint16_t>(entity_fields.size()),
        .fields = entity_fields.data(),
    });

    property_fields = {
        MakeField("m_vecModifiers", list_type, 0),
        MakeField("m_bPredictedOwner", bool_type, 24),
    };
    classes.push_back(schema::ClassInfo{
        .name = "CModifierProperty",
        .size = 32,
        .field_count = static_cast<uint16_t>(property_fields.size()),
        .fields = property_fields.data(),
    });

    vdata_fields = {
        MakeField("m_flDuration", duration_type, 8),
    };
    classes.push_back(schema::ClassInfo{
        .name = "CModifierVData",
        .size = 24,
        .field_count = static_cast<uint16_t>(vdata_fields.size()),
        .fields = vdata_fields.data(),
    });
    for (auto& info : classes) {
      info.self = &info;
    }
  }

  std::array<void*, 16> system_methods{};
  std::array<void*, 8> scope_methods{};
  schema::DeclaredType pointer_type{};
  schema::DeclaredType byte_type{};
  schema::DeclaredType list_type{};
  schema::DeclaredType bool_type{};
  schema::DeclaredType duration_type{};
  std::array<schema::Field, 2> entity_fields{};
  std::array<schema::Field, 2> property_fields{};
  std::array<schema::Field, 1> vdata_fields{};
  std::vector<schema::ClassInfo> classes;
  Scope scope{scope_methods.data(), this};
  System system{system_methods.data(), &scope};
};

struct CallState {
  std::array<unsigned char, 32> definition{};
  std::array<unsigned char, 32> instance{};
  std::vector<unsigned char*> active;
  int32_t last_scope = 0;
  uint32_t last_lookup_hash = 0;
  void* last_property = nullptr;
  void* last_caster = nullptr;
  uint32_t last_ability_handle = 0;
  uint32_t last_ability_subclass = 0;
  void* last_definition = nullptr;
  uint8_t predicted_during_add = 0;
};

CallState g_state;

void* FakeLookup(int32_t scope, uint32_t id) {
  g_state.last_scope = scope;
  g_state.last_lookup_hash = id;
  return g_state.definition.data();
}

void* FakeAdd(void* property, void* caster, uint32_t ability_handle, uint32_t ability_subclass,
              void* definition, void* params, void* kv) {
  EXPECT_EQ(params, nullptr);
  EXPECT_EQ(kv, nullptr);
  g_state.last_property = property;
  g_state.last_caster = caster;
  g_state.last_ability_handle = ability_handle;
  g_state.last_ability_subclass = ability_subclass;
  g_state.last_definition = definition;
  g_state.predicted_during_add = static_cast<unsigned char*>(property)[24];

  std::memcpy(g_state.instance.data() + 0x10, &definition, sizeof(definition));
  g_state.active.push_back(g_state.instance.data());
  const UtlVector<unsigned char*> vec{
      .count = static_cast<int32_t>(g_state.active.size()),
      .padding = 0,
      .data = g_state.active.data(),
      .capacity = static_cast<uint32_t>(g_state.active.size()),
      .flags = 0,
  };
  std::memcpy(property, &vec, sizeof(vec));
  return g_state.instance.data();
}

TEST(AbilityModifier, AttachesEmbeddedAndStandaloneModifiersToEntitiesAndRestoresDuration) {
  // Prepare a live entity and its modifier property.
  ModifierSchema schema;
  g_state = CallState{};
  const float stock_duration = 6.0f;
  std::memcpy(g_state.definition.data() + 8, &stock_duration, sizeof(stock_duration));

  std::array<unsigned char, 32> property{};
  std::array<unsigned char, 24> entity{};
  unsigned char* property_ptr = property.data();
  std::memcpy(entity.data() + 16, &property_ptr, sizeof(property_ptr));

  // Resolve an embedded modifier and attach it without an owned ability handle.
  auto modifier = AbilityModifier::Resolve(FakeLookup, FakeAdd, &schema.system,
                                           "upgrade_colossus/modifier_colossus_active", -1.0f);
  ASSERT_TRUE(modifier) << modifier.error();
  EXPECT_EQ(g_state.last_scope, 2);
  EXPECT_EQ(g_state.last_lookup_hash,
            MakeMemberName("upgrade_colossus/modifier_colossus_active").hash);

  float active_duration = 0;
  std::memcpy(&active_duration, g_state.definition.data() + 8, sizeof(active_duration));
  EXPECT_FLOAT_EQ(active_duration, -1.0f);

  const auto before = modifier->AttachedToEntity(entity.data());
  ASSERT_TRUE(before) << before.error();
  EXPECT_FALSE(*before);

  const auto attached = modifier->AttachEntity(entity.data());
  ASSERT_TRUE(attached) << attached.error();
  EXPECT_EQ(g_state.last_property, property.data());
  EXPECT_EQ(g_state.last_caster, entity.data());
  EXPECT_EQ(g_state.last_ability_handle, kInvalidEHandleIndex);
  EXPECT_EQ(g_state.last_ability_subclass, MakeMemberName("upgrade_colossus").hash);
  EXPECT_EQ(g_state.last_definition, g_state.definition.data());
  EXPECT_EQ(g_state.predicted_during_add, 1u);
  EXPECT_EQ(property[24], 0u);

  const auto after = modifier->AttachedToEntity(entity.data());
  ASSERT_TRUE(after) << after.error();
  EXPECT_TRUE(*after);

  modifier->Restore();
  std::memcpy(&active_duration, g_state.definition.data() + 8, sizeof(active_duration));
  EXPECT_FLOAT_EQ(active_duration, stock_duration);

  // A standalone modifier passes zero for the ability subclass hash.
  auto standalone = AbilityModifier::Resolve(FakeLookup, FakeAdd, &schema.system,
                                             "modifier_citadel_idol_glow", 10.0f);
  ASSERT_TRUE(standalone) << standalone.error();
  ASSERT_TRUE(standalone->AttachEntity(entity.data()));
  EXPECT_EQ(g_state.last_ability_subclass, 0u);
  standalone->Restore();
}

TEST(AbilityModifier, RefusesDeadEntitiesAndEntitiesWithoutModifierProperty) {
  ModifierSchema schema;
  g_state = CallState{};
  auto modifier = AbilityModifier::Resolve(FakeLookup, FakeAdd, &schema.system,
                                           "modifier_citadel_idol_glow", 5.0f);
  ASSERT_TRUE(modifier) << modifier.error();

  std::array<unsigned char, 32> property{};
  std::array<unsigned char, 24> entity{};
  EXPECT_FALSE(modifier->AttachEntity(entity.data()));
  EXPECT_FALSE(modifier->AttachedToEntity(entity.data()));

  unsigned char* property_ptr = property.data();
  std::memcpy(entity.data() + 16, &property_ptr, sizeof(property_ptr));
  entity[8] = 2;  // LIFE_DEAD
  EXPECT_FALSE(modifier->EntityAlive(entity.data()));
  EXPECT_FALSE(modifier->AttachEntity(entity.data()));
}

}  // namespace
