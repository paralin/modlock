// Contract tests for manifest definition changes: float, range-float,
// modifier-level-float, map, modifier script value and copied fields write
// into parsed definitions and restore cleanly through DefinitionUndo.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "gameinterop/schema_layout.h"
#include "gameinterop/utl_vector.h"
#include "gtest/gtest.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "wasm/definition_changes.h"

namespace {

namespace schema = modlock::gameinterop::schema;
using modlock::gameinterop::FindVirtualSlot;
using modlock::gameinterop::UtlVector;
using modlock::wasm::ChangeDefinition;
using modlock::wasm::Definition;
using modlock::wasm::DefinitionUndo;

// Field builds one schema::Field pointing at declared.
schema::Field MakeField(const char* name, const schema::DeclaredType& declared, int32_t offset) {
  return schema::Field{
      .name = name, .type = reinterpret_cast<const schema::Type*>(&declared), .offset = offset};
}

// FakeSchema serves declared classes and enums through ISchemaSystem's vtable
// ABI so ChangeDefinition exercises real SchemaFieldOf / SchemaEnumValueOf
// resolution on every platform.
struct FakeSchema {
  struct Scope {
    void** vtable;
    FakeSchema* owner;
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

  static void FindEnum(void* self, void* out, const char* name) {
    auto* owner = static_cast<Scope*>(self)->owner;
    for (const auto& info : owner->enums) {
      if (std::string_view(info.name) == name) {
        *static_cast<const void**>(out) = &info;
        return;
      }
    }
    *static_cast<void**>(out) = nullptr;
  }

  FakeSchema() {
    // Wire the schema system and type scope vtables.
    system_methods[FindVirtualSlot("schema-system.find-type-scope")->index] =
        reinterpret_cast<void*>(&FindScope);
    scope_methods[FindVirtualSlot("schema-type-scope.find-declared-class")->index] =
        reinterpret_cast<void*>(&FindClass);
    scope_methods[FindVirtualSlot("schema-type-scope.find-declared-enum")->index] =
        reinterpret_cast<void*>(&FindEnum);

    // Declare the field types used across modifier and hero definitions.
    float_type.name = "float32";
    level_float_type.name = "CModifierLevelFloat";
    range_float_type.name = "CRangeFloat";
    script_list_type.name = "CUtlVector< ModifierScriptValue_t >";
    enum_type.name = "int32";
    map_type.name = "CUtlMap< EStatsType, float32 >";
    particle_type.name =
        "CResourceNameTyped< CWeakHandle< InfoForResourceTypeIParticleSystemDefinition > >";

    // Declare ModifierScriptValue_t (key at +0, 4-level float at +8, stride 32).
    script_fields = {
        MakeField("m_eModifierValue", enum_type, 0),
        MakeField("m_value", level_float_type, 8),
    };
    classes.push_back(schema::ClassInfo{
        .name = "ModifierScriptValue_t",
        .size = 32,
        .field_count = static_cast<uint16_t>(script_fields.size()),
        .fields = script_fields.data(),
    });

    // Declare CModifierVData_SetModelScale with scalar, level, range, script
    // list and particle fields.
    modifier_fields = {
        MakeField("m_flDuration", level_float_type, 0),
        MakeField("m_flScale", range_float_type, 16),
        MakeField("m_flLingeringAssistWindow", float_type, 24),
        MakeField("m_vecScriptValues", script_list_type, 32),
        MakeField("m_strParticleEffect", particle_type, 56),
        MakeField("m_strParticleStatusEffect", particle_type, 72),
    };
    classes.push_back(schema::ClassInfo{
        .name = "CModifierVData_SetModelScale",
        .size = 88,
        .field_count = static_cast<uint16_t>(modifier_fields.size()),
        .fields = modifier_fields.data(),
    });

    // Declare CitadelHeroData_t with an enum-keyed starting stats map.
    hero_fields = {
        MakeField("m_mapStartingStats", map_type, 0),
    };
    classes.push_back(schema::ClassInfo{
        .name = "CitadelHeroData_t",
        .size = 32,
        .field_count = static_cast<uint16_t>(hero_fields.size()),
        .fields = hero_fields.data(),
    });
    for (auto& info : classes) {
      info.self = &info;
    }

    // Declare EModifierValue and EStatsType.
    modifier_values = {
        schema::Enumerator{.name = "MODIFIER_VALUE_MODEL_SCALE", .value = 42},
        schema::Enumerator{.name = "MODIFIER_VALUE_MOVEMENT_SPEED_MAX", .value = 99},
    };
    stat_values = {
        schema::Enumerator{.name = "EMaxHealth", .value = 7},
    };
    enums.push_back(schema::EnumInfo{
        .name = "EModifierValue",
        .size = 2,
        .enumerator_count = static_cast<uint16_t>(modifier_values.size()),
        .enumerators = modifier_values.data(),
    });
    enums.push_back(schema::EnumInfo{
        .name = "EStatsType",
        .size = 4,
        .enumerator_count = static_cast<uint16_t>(stat_values.size()),
        .enumerators = stat_values.data(),
    });
    for (auto& info : enums) {
      info.self = &info;
    }
  }

  std::array<void*, 16> system_methods{};
  std::array<void*, 8> scope_methods{};
  schema::DeclaredType float_type{};
  schema::DeclaredType level_float_type{};
  schema::DeclaredType range_float_type{};
  schema::DeclaredType script_list_type{};
  schema::DeclaredType enum_type{};
  schema::DeclaredType map_type{};
  schema::DeclaredType particle_type{};
  std::array<schema::Field, 2> script_fields{};
  std::array<schema::Field, 6> modifier_fields{};
  std::array<schema::Field, 1> hero_fields{};
  std::vector<schema::ClassInfo> classes;
  std::array<schema::Enumerator, 2> modifier_values{};
  std::array<schema::Enumerator, 1> stat_values{};
  std::vector<schema::EnumInfo> enums;
  Scope scope{scope_methods.data(), this};
  System system{system_methods.data(), &scope};
};

TEST(DefinitionChanges, WritesModifierFloatsScriptValuesCopiesAndRestoresUndo) {
  // Lay out a modifier definition with distinct initial values across all 4
  // levels of m_flDuration, both bounds of m_flScale, a scalar float, a
  // ModifierScriptValue_t entry, and two 16-byte particle fields.
  FakeSchema schema;
  std::array<unsigned char, 88> storage{};
  const std::array<float, 4> initial_duration{1.0f, 2.0f, 3.0f, 4.0f};
  const std::array<float, 2> initial_scale{0.8f, 1.2f};
  const float initial_window = 5.0f;
  std::memcpy(storage.data() + 0, initial_duration.data(), sizeof(initial_duration));
  std::memcpy(storage.data() + 16, initial_scale.data(), sizeof(initial_scale));
  std::memcpy(storage.data() + 24, &initial_window, sizeof(initial_window));

  std::array<unsigned char, 32> script_entry{};
  const uint16_t model_scale_enum = 42;
  const std::array<unsigned char, 2> uninitialized_padding{0xAA, 0xBB};
  const std::array<float, 4> initial_script_scale{1.2f, 1.2f, 1.2f, 1.2f};
  std::memcpy(script_entry.data() + 0, &model_scale_enum, sizeof(model_scale_enum));
  std::memcpy(script_entry.data() + 2, uninitialized_padding.data(),
              sizeof(uninitialized_padding));
  std::memcpy(script_entry.data() + 8, initial_script_scale.data(), sizeof(initial_script_scale));
  const UtlVector<unsigned char> script_list{
      .count = 1, .padding = 0, .data = script_entry.data(), .capacity = 1, .flags = 0};
  std::memcpy(storage.data() + 32, &script_list, sizeof(script_list));

  const std::array<unsigned char, 16> particle_a{
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const std::array<unsigned char, 16> particle_b{
      99, 98, 97, 96, 95, 94, 93, 92, 91, 90, 89, 88, 87, 86, 85, 84};
  std::memcpy(storage.data() + 56, particle_a.data(), sizeof(particle_a));
  std::memcpy(storage.data() + 72, particle_b.data(), sizeof(particle_b));

  // Apply a FILE_MODIFIERS definition change.
  const std::string script_path =
      "CModifierVData_SetModelScale.m_vecScriptValues[EModifierValue.MODIFIER_VALUE_MODEL_SCALE]";
  Definition change;
  change.set_file(Definition::FILE_MODIFIERS);
  change.set_entry("upgrade_colossus/modifier_colossus_active");
  (*change.mutable_fields())["CModifierVData_SetModelScale.m_flDuration"] = 12.5f;
  (*change.mutable_fields())["CModifierVData_SetModelScale.m_flScale"] = 2.25f;
  (*change.mutable_fields())["CModifierVData_SetModelScale.m_flLingeringAssistWindow"] = 9.0f;
  (*change.mutable_fields())[script_path] = 1.75f;
  (*change.mutable_copy_fields())["CModifierVData_SetModelScale.m_strParticleEffect"] =
      "CModifierVData_SetModelScale.m_strParticleStatusEffect";

  DefinitionUndo undo;
  const auto problems =
      ChangeDefinition(change, storage.data(), &schema.system, "server.dll", undo);
  ASSERT_TRUE(problems.empty()) << problems.front();

  // Verify every element of every field was updated.
  std::array<float, 4> duration{};
  std::array<float, 2> scale{};
  float window = 0;
  std::array<float, 4> script_scale{};
  std::array<unsigned char, 16> copied_particle{};
  std::memcpy(duration.data(), storage.data() + 0, sizeof(duration));
  std::memcpy(scale.data(), storage.data() + 16, sizeof(scale));
  std::memcpy(&window, storage.data() + 24, sizeof(window));
  std::memcpy(script_scale.data(), script_entry.data() + 8, sizeof(script_scale));
  std::memcpy(copied_particle.data(), storage.data() + 56, sizeof(copied_particle));
  EXPECT_EQ(duration, (std::array<float, 4>{12.5f, 12.5f, 12.5f, 12.5f}));
  EXPECT_EQ(scale, (std::array<float, 2>{2.25f, 2.25f}));
  EXPECT_FLOAT_EQ(window, 9.0f);
  EXPECT_EQ(script_scale, (std::array<float, 4>{1.75f, 1.75f, 1.75f, 1.75f}));
  EXPECT_EQ(copied_particle, particle_b);

  // ForgetWorld drops hero entries without touching modifier entries, and
  // Restore puts every original modifier byte back.
  std::array<unsigned char, 32> hero_storage{};
  std::array<unsigned char, 24> map_node{};
  const int32_t left = -1;
  const int32_t max_health_key = 7;
  const float initial_health = 550.0f;
  std::memcpy(map_node.data(), &left, sizeof(left));
  std::memcpy(map_node.data() + 16, &max_health_key, sizeof(max_health_key));
  std::memcpy(map_node.data() + 20, &initial_health, sizeof(initial_health));
  const int32_t map_count = 1;
  unsigned char* nodes = map_node.data();
  std::memcpy(hero_storage.data() + 8, &map_count, sizeof(map_count));
  std::memcpy(hero_storage.data() + 16, &nodes, sizeof(nodes));

  Definition hero_change;
  hero_change.set_file(Definition::FILE_HEROES);
  hero_change.set_entry("hero_priest");
  (*hero_change.mutable_fields())["CitadelHeroData_t.m_mapStartingStats[EStatsType.EMaxHealth]"] =
      1000.0f;
  ASSERT_TRUE(
      ChangeDefinition(hero_change, hero_storage.data(), &schema.system, "server.dll", undo)
          .empty());

  undo.ForgetWorld();
  undo.Restore();
  float hero_health = 0;
  std::memcpy(&hero_health, map_node.data() + 20, sizeof(hero_health));
  EXPECT_FLOAT_EQ(hero_health, 1000.0f);

  std::memcpy(duration.data(), storage.data() + 0, sizeof(duration));
  std::memcpy(scale.data(), storage.data() + 16, sizeof(scale));
  std::memcpy(&window, storage.data() + 24, sizeof(window));
  std::memcpy(script_scale.data(), script_entry.data() + 8, sizeof(script_scale));
  std::memcpy(copied_particle.data(), storage.data() + 56, sizeof(copied_particle));
  EXPECT_EQ(duration, initial_duration);
  EXPECT_EQ(scale, initial_scale);
  EXPECT_FLOAT_EQ(window, initial_window);
  EXPECT_EQ(script_scale, initial_script_scale);
  EXPECT_EQ(copied_particle, particle_a);
}

TEST(DefinitionChanges, ReportsNonFloatAndMissingScriptValueKeys) {
  // A non-float field and an absent EModifierValue key are reported without
  // writing.
  FakeSchema schema;
  std::array<unsigned char, 88> storage{};
  std::array<unsigned char, 32> script_entry{};
  const int32_t model_scale_enum = 42;
  std::memcpy(script_entry.data(), &model_scale_enum, sizeof(model_scale_enum));
  const UtlVector<unsigned char> script_list{
      .count = 1, .padding = 0, .data = script_entry.data(), .capacity = 1, .flags = 0};
  std::memcpy(storage.data() + 32, &script_list, sizeof(script_list));

  const std::string missing_key =
      "CModifierVData_SetModelScale.m_vecScriptValues"
      "[EModifierValue.MODIFIER_VALUE_MOVEMENT_SPEED_MAX]";
  Definition change;
  change.set_file(Definition::FILE_MODIFIERS);
  change.set_entry("modifier_test");
  (*change.mutable_fields())["CModifierVData_SetModelScale.m_strParticleEffect"] = 1.0f;
  (*change.mutable_fields())[missing_key] = 500.0f;

  DefinitionUndo undo;
  const auto problems =
      ChangeDefinition(change, storage.data(), &schema.system, "server.dll", undo);
  ASSERT_EQ(problems.size(), 2u);
}

}  // namespace
