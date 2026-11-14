// Contract tests for manifest definition text fields: symbol, CUtlString,
// sound event and Panorama image fields write into parsed definitions with
// {addon} expansion, restore cleanly through DefinitionUndo, and refuse script
// and non-text fields.
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "fake_schema_system.h"
#include "gameinterop/schema_layout.h"
#include "gtest/gtest.h"
#include "wasm/definition_changes.h"

namespace {

namespace schema = modlock::gameinterop::schema;
using modlock::test::MakeSchemaField;
using modlock::wasm::ChangeDefinition;
using modlock::wasm::Definition;
using modlock::wasm::DefinitionText;
using modlock::wasm::DefinitionUndo;

// BufferStringView mirrors the 16-byte CBufferString layout of CSoundEventName
// and CPanoramaImageName.
struct BufferStringView {
  uint32_t length;
  uint32_t allocated;
  const char* chars;
};

static_assert(sizeof(BufferStringView) == 16);

// FakeAllocate stands in for the game's allocator, keeping each allocation
// so a test can tell its memory apart.
void* FakeAllocate(size_t size) {
  static auto* allocations = new std::vector<std::unique_ptr<char[]>>();
  return allocations->emplace_back(std::make_unique<char[]>(size)).get();
}

// FakeInternSymbol stands in for the game's global symbol table.
const char* FakeInternSymbol(const char* text) {
  static auto* symbols = new std::set<std::string>();
  return symbols->insert(text).first->c_str();
}

// kGameText writes text through the fakes, with td-3f9a01c2 as the addon.
const DefinitionText kGameText{
    .addon = "td-3f9a01c2", .allocate = &FakeAllocate, .intern_symbol = &FakeInternSymbol};

// kNoGame writes text where the game's allocator and symbol table are
// unavailable.
const DefinitionText kNoGame{.addon = {},
                             .allocate = [](size_t) -> void* { return nullptr; },
                             .intern_symbol = [](const char*) -> const char* { return nullptr; }};

// FakeSchema declares the classes and enums the tests below change.
struct FakeSchema : modlock::test::FakeSchemaSystem {
  FakeSchema() {
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
        MakeSchemaField("m_strTag", utl_string_type, 0),
        MakeSchemaField("m_iszPrivateVScripts", symbol_type, 8),
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
        MakeSchemaField("m_iszAbilityName", symbol_type, 0),
        MakeSchemaField("m_strAG2SourceName", global_symbol_type, 8),
        MakeSchemaField("m_strCSSClass", utl_string_type, 16),
        MakeSchemaField("m_strHintText", utl_string_type, 24),
        MakeSchemaField("m_strCastSound", sound_type, 32),
        MakeSchemaField("m_strChannelLoopSound", sound_type, 48),
        MakeSchemaField("m_strAbilityImage", image_type, 64),
        MakeSchemaField("m_embedded", embedded_type, 80),
        MakeSchemaField("m_flCooldown", float_type, 96),
        MakeSchemaField("m_ExplosionParticle", particle_type, 104),
    };
    classes.push_back(schema::ClassInfo{
        .name = "CitadelAbilityVData",
        .size = 328,
        .field_count = static_cast<uint16_t>(ability_fields.size()),
        .fields = ability_fields.data(),
    });

    // Declare CitadelHeroData_t with a CUtlString and an enum-keyed map.
    hero_fields = {
        MakeSchemaField("m_strHeroSortName", utl_string_type, 0),
        MakeSchemaField("m_mapStartingStats", map_type, 8),
    };
    classes.push_back(schema::ClassInfo{
        .name = "CitadelHeroData_t",
        .size = 32,
        .field_count = static_cast<uint16_t>(hero_fields.size()),
        .fields = hero_fields.data(),
    });

    // Declare EStatsType for indexed map rejection checks.
    stat_values = {
        schema::Enumerator{.name = "EMaxHealth", .value = 7},
    };
    enums.push_back(schema::EnumInfo{
        .name = "EStatsType",
        .enumerator_count = static_cast<uint16_t>(stat_values.size()),
        .enumerators = stat_values.data(),
    });
    Seal();
  }

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
  std::vector<schema::Enumerator> stat_values;
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
  const BufferStringView orig_cast{.length = 9, .allocated = 0x8000000au, .chars = "Old.Sound"};
  const BufferStringView orig_loop{.length = 10, .allocated = 0x8000000bu, .chars = "Loop.Sound"};
  const BufferStringView orig_image{.length = 0, .allocated = 0xc0000008u, .chars = nullptr};
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
  text["CitadelAbilityVData.m_strAbilityImage"] = "file://{images}/hud/{addon}/launcher.png";
  text["CitadelAbilityVData.m_embedded/EmbeddedText_t.m_strTag"] = "nested_{addon}";

  DefinitionUndo undo;
  const auto problems =
      ChangeDefinition(change, storage.data(), &schema.system, "server.dll", undo, kGameText);
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
  EXPECT_EQ(global, FakeInternSymbol("hero_warden"));
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
  ASSERT_TRUE(ChangeDefinition(hero_change, hero_storage.data(), &schema.system, "server.dll", undo,
                               kGameText)
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

  // A CUtlString without the game's allocator, or a CGlobalSymbol without its
  // symbol table, is reported rather than pointed at memory the game cannot
  // free or compare.
  Definition native_change;
  native_change.set_file(Definition::FILE_ABILITIES);
  native_change.set_entry("gunslinger_rocket_launcher");
  (*native_change.mutable_text_fields())["CitadelAbilityVData.m_strCSSClass"] = "tower";
  (*native_change.mutable_text_fields())["CitadelAbilityVData.m_strAG2SourceName"] = "hero_warden";
  const auto native_problems =
      ChangeDefinition(native_change, storage.data(), &schema.system, "server.dll", undo, kNoGame);
  EXPECT_EQ(native_problems.size(), 2u);
  EXPECT_EQ(storage, snapshot);
}

}  // namespace
