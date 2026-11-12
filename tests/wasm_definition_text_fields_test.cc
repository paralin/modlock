// Contract tests for manifest definition text fields: symbol, CUtlString,
// sound event and Panorama image fields write into parsed definitions with
// {addon} expansion, restore cleanly through DefinitionUndo, and refuse script
// and non-text fields.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "gameinterop/schema_layout.h"
#include "gtest/gtest.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "wasm/definition_changes.h"

namespace {

namespace schema = modlock::gameinterop::schema;
using modlock::gameinterop::FindVirtualSlot;
using modlock::wasm::ChangeDefinition;
using modlock::wasm::Definition;
using modlock::wasm::DefinitionUndo;

// BufferStringView mirrors the 16-byte CBufferString layout of CSoundEventName
// and CPanoramaImageName.
struct BufferStringView {
  uint32_t length;
  uint32_t allocated;
  const char* chars;
};
static_assert(sizeof(BufferStringView) == 16);

// MakeField builds one schema::Field pointing at declared.
schema::Field MakeField(const char* name, const schema::DeclaredType& declared, int32_t offset) {
  return schema::Field{
      .name = name, .type = reinterpret_cast<const schema::Type*>(&declared), .offset = offset};
}

// FakeSchema serves declared classes and enums through ISchemaSystem's vtable
// ABI so ChangeDefinition exercises real SchemaFieldOf resolution on every
// platform.
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

    // Declare the field types used across ability and hero definitions.
    symbol_type.name = "CUtlSymbolLarge";
    global_symbol_type.name = "CGlobalSymbol";
    utl_string_type.name = "CUtlString";
    sound_type.name = "CSoundEventName";
    image_type.name = "CPanoramaImageName";
    float_type.name = "float32";
    map_type.name = "CUtlMap< EStatsType, float32 >";
    particle_type.name =
        "CResourceNameTyped< CWeakHandle< InfoForResourceTypeIParticleSystemDefinition > >";
    embedded_type.name = "EmbeddedText_t";

    // Declare EmbeddedText_t with a CUtlString tag and a refused script field.
    embedded_fields = {
        MakeField("m_strTag", utl_string_type, 0),
        MakeField("m_iszPrivateVScripts", symbol_type, 8),
    };
    classes.push_back(schema::ClassInfo{
        .name = "EmbeddedText_t",
        .size = 16,
        .field_count = static_cast<uint16_t>(embedded_fields.size()),
        .fields = embedded_fields.data(),
    });

    // Declare CitadelAbilityVData with every supported text field type plus
    // non-text fields.
    ability_fields = {
        MakeField("m_iszAbilityName", symbol_type, 0),
        MakeField("m_strAG2SourceName", global_symbol_type, 8),
        MakeField("m_strCSSClass", utl_string_type, 16),
        MakeField("m_strHintText", utl_string_type, 24),
        MakeField("m_strCastSound", sound_type, 32),
        MakeField("m_strChannelLoopSound", sound_type, 48),
        MakeField("m_strAbilityImage", image_type, 64),
        MakeField("m_embedded", embedded_type, 80),
        MakeField("m_flCooldown", float_type, 96),
        MakeField("m_ExplosionParticle", particle_type, 104),
    };
    classes.push_back(schema::ClassInfo{
        .name = "CitadelAbilityVData",
        .size = 328,
        .field_count = static_cast<uint16_t>(ability_fields.size()),
        .fields = ability_fields.data(),
    });

    // Declare CitadelHeroData_t with a CUtlString and an enum-keyed map.
    hero_fields = {
        MakeField("m_strHeroSortName", utl_string_type, 0),
        MakeField("m_mapStartingStats", map_type, 8),
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

    // Declare EStatsType for indexed map rejection checks.
    stat_values = {
        schema::Enumerator{.name = "EMaxHealth", .value = 7},
    };
    enums.push_back(schema::EnumInfo{
        .name = "EStatsType",
        .enumerator_count = static_cast<uint16_t>(stat_values.size()),
        .enumerators = stat_values.data(),
    });
    for (auto& info : enums) {
      info.self = &info;
    }

    scope = Scope{.vtable = scope_methods.data(), .owner = this};
    system = System{.vtable = system_methods.data(), .scope = &scope};
  }

  std::array<void*, 16> system_methods{};
  std::array<void*, 16> scope_methods{};
  schema::DeclaredType symbol_type{};
  schema::DeclaredType global_symbol_type{};
  schema::DeclaredType utl_string_type{};
  schema::DeclaredType sound_type{};
  schema::DeclaredType image_type{};
  schema::DeclaredType float_type{};
  schema::DeclaredType map_type{};
  schema::DeclaredType particle_type{};
  schema::DeclaredType embedded_type{};
  std::vector<schema::Field> embedded_fields;
  std::vector<schema::Field> ability_fields;
  std::vector<schema::Field> hero_fields;
  std::vector<schema::ClassInfo> classes;
  std::vector<schema::Enumerator> stat_values;
  std::vector<schema::EnumInfo> enums;
  Scope scope{};
  System system{};
};

TEST(DefinitionTextFields, WritesTextFieldsExpandsAddonAndRestoresBytes) {
  // Populate initial ability VData bytes for every supported text field kind.
  FakeSchema schema;
  std::array<unsigned char, 328> storage{};
  const char* orig_symbol = "ability_old";
  const char* orig_global = "ag2_old";
  const char* orig_css = "old_class";
  const char* orig_hint = "old_hint";
  const char* orig_tag = "old_tag";
  const BufferStringView orig_cast{
      .length = 9, .allocated = 0x8000000au, .chars = "Old.Sound"};
  const BufferStringView orig_loop{
      .length = 10, .allocated = 0x8000000bu, .chars = "Loop.Sound"};
  const BufferStringView orig_image{
      .length = 0, .allocated = 0xc0000008u, .chars = nullptr};
  std::memcpy(storage.data() + 0, &orig_symbol, sizeof(orig_symbol));
  std::memcpy(storage.data() + 8, &orig_global, sizeof(orig_global));
  std::memcpy(storage.data() + 16, &orig_css, sizeof(orig_css));
  std::memcpy(storage.data() + 24, &orig_hint, sizeof(orig_hint));
  std::memcpy(storage.data() + 32, &orig_cast, sizeof(orig_cast));
  std::memcpy(storage.data() + 48, &orig_loop, sizeof(orig_loop));
  std::memcpy(storage.data() + 64, &orig_image, sizeof(orig_image));
  std::memcpy(storage.data() + 80, &orig_tag, sizeof(orig_tag));
  const auto snapshot = storage;

  // Apply text field overrides with {addon} substitution and empty clears.
  Definition change;
  change.set_file(Definition::FILE_ABILITIES);
  change.set_entry("gunslinger_rocket_launcher");
  auto& text = *change.mutable_text_fields();
  text["CitadelAbilityVData.m_iszAbilityName"] = "custom_{addon}_launcher";
  text["CitadelAbilityVData.m_strAG2SourceName"] = "hero_warden";
  text["CitadelAbilityVData.m_strCSSClass"] = "tower_{addon}";
  text["CitadelAbilityVData.m_strHintText"] = "";
  text["CitadelAbilityVData.m_strCastSound"] = "Hero.Warden.Flask.Throw";
  text["CitadelAbilityVData.m_strChannelLoopSound"] = "";
  text["CitadelAbilityVData.m_strAbilityImage"] =
      "file://{images}/hud/{addon}/launcher.png";
  text["CitadelAbilityVData.m_embedded/EmbeddedText_t.m_strTag"] = "nested_{addon}";

  DefinitionUndo undo;
  const auto problems =
      ChangeDefinition(change, storage.data(), &schema.system, "server.dll", undo, "td-3f9a01c2");
  ASSERT_TRUE(problems.empty()) << problems.front();

  // Verify 8-byte symbol and CUtlString pointers.
  const char* symbol = nullptr;
  const char* global = nullptr;
  const char* css = nullptr;
  const char* hint = reinterpret_cast<const char*>(1);
  const char* tag = nullptr;
  std::memcpy(&symbol, storage.data() + 0, sizeof(symbol));
  std::memcpy(&global, storage.data() + 8, sizeof(global));
  std::memcpy(&css, storage.data() + 16, sizeof(css));
  std::memcpy(&hint, storage.data() + 24, sizeof(hint));
  std::memcpy(&tag, storage.data() + 80, sizeof(tag));
  ASSERT_NE(symbol, nullptr);
  ASSERT_NE(global, nullptr);
  ASSERT_NE(css, nullptr);
  ASSERT_NE(tag, nullptr);
  EXPECT_STREQ(symbol, "custom_td-3f9a01c2_launcher");
  EXPECT_STREQ(global, "hero_warden");
  EXPECT_STREQ(css, "tower_td-3f9a01c2");
  EXPECT_EQ(hint, nullptr);
  EXPECT_STREQ(tag, "nested_td-3f9a01c2");

  // Verify 16-byte CBufferString sound and Panorama image headers.
  BufferStringView cast{};
  BufferStringView loop{};
  BufferStringView image{};
  std::memcpy(&cast, storage.data() + 32, sizeof(cast));
  std::memcpy(&loop, storage.data() + 48, sizeof(loop));
  std::memcpy(&image, storage.data() + 64, sizeof(image));
  const std::string_view expected_sound = "Hero.Warden.Flask.Throw";
  EXPECT_EQ(cast.length, expected_sound.size());
  EXPECT_EQ(cast.allocated, 0x80000000u | static_cast<uint32_t>(expected_sound.size() + 1));
  ASSERT_NE(cast.chars, nullptr);
  EXPECT_STREQ(cast.chars, expected_sound.data());

  EXPECT_EQ(loop.length, 0u);
  EXPECT_EQ(loop.allocated, 0xc0000008u);
  EXPECT_EQ(loop.chars, nullptr);

  const std::string_view expected_image = "file://{images}/hud/td-3f9a01c2/launcher.png";
  EXPECT_EQ(image.length, expected_image.size());
  EXPECT_EQ(image.allocated, 0x80000000u | static_cast<uint32_t>(expected_image.size() + 1));
  ASSERT_NE(image.chars, nullptr);
  EXPECT_STREQ(image.chars, expected_image.data());

  // ForgetWorld drops hero entries without touching ability entries, and
  // Restore puts every original ability byte back.
  std::array<unsigned char, 32> hero_storage{};
  const char* orig_hero_sort = "priest";
  std::memcpy(hero_storage.data(), &orig_hero_sort, sizeof(orig_hero_sort));
  Definition hero_change;
  hero_change.set_file(Definition::FILE_HEROES);
  hero_change.set_entry("hero_priest");
  (*hero_change.mutable_text_fields())["CitadelHeroData_t.m_strHeroSortName"] = "warden";
  ASSERT_TRUE(
      ChangeDefinition(hero_change, hero_storage.data(), &schema.system, "server.dll", undo)
          .empty());

  undo.ForgetWorld();
  undo.Restore();
  EXPECT_EQ(storage, snapshot);

  const char* hero_sort = nullptr;
  std::memcpy(&hero_sort, hero_storage.data(), sizeof(hero_sort));
  ASSERT_NE(hero_sort, nullptr);
  EXPECT_STREQ(hero_sort, "warden");
}

TEST(DefinitionTextFields, RefusesScriptFieldsAndNonTextTypes) {
  // Script fields, numeric fields, resource handles and indexed map paths are
  // reported without writing into storage.
  FakeSchema schema;
  std::array<unsigned char, 328> storage{};
  storage.fill(0x5a);
  const auto snapshot = storage;

  Definition change;
  change.set_file(Definition::FILE_ABILITIES);
  change.set_entry("gunslinger_rocket_launcher");
  auto& text = *change.mutable_text_fields();
  text["CitadelAbilityVData.m_embedded/EmbeddedText_t.m_iszPrivateVScripts"] = "evil.lua";
  text["CitadelAbilityVData.m_embedded.m_iszPrivateVScripts"] = "evil2.lua";
  text["CitadelAbilityVData.m_flCooldown"] = "1.5";
  text["CitadelAbilityVData.m_ExplosionParticle"] = "particles/boom.vpcf";

  DefinitionUndo undo;
  const auto problems =
      ChangeDefinition(change, storage.data(), &schema.system, "server.dll", undo);
  EXPECT_EQ(problems.size(), 4u);
  EXPECT_EQ(storage, snapshot);

  std::array<unsigned char, 32> hero_storage{};
  hero_storage.fill(0x3c);
  const auto hero_snapshot = hero_storage;
  const std::string stat_path = "CitadelHeroData_t.m_mapStartingStats[EStatsType.EMaxHealth]";
  Definition hero_change;
  hero_change.set_file(Definition::FILE_HEROES);
  hero_change.set_entry("hero_priest");
  (*hero_change.mutable_text_fields())[stat_path] = "1000";
  const auto hero_problems =
      ChangeDefinition(hero_change, hero_storage.data(), &schema.system, "server.dll", undo);
  EXPECT_EQ(hero_problems.size(), 1u);
  EXPECT_EQ(hero_storage, hero_snapshot);
}

}  // namespace
