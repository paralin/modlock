// Contract tests for the gameinterop hero-definition lookup: recorded signature
// resolution against a synthetic module image, name -> (id, definition)
// lookup through recording fakes, and the fail-closed contracts (unknown
// name, unlive manager, unresolved slots).
#include <string>
#include <string_view>

#include "gameinterop_module_image_test.h"
#include "gtest/gtest.h"
#include "modlock/gameinterop/hero_definitions.h"

namespace {

using modlock::gameinterop::HeroDefinitionCalls;
using modlock::gameinterop::HeroDefinitions;
namespace gti = modlock::gameinterop::testing;

// Recording fakes preserve the raw calling conventions of the native probes.

void* g_manager_sentinel = reinterpret_cast<void*>(0x1400);
void* g_hero_def_sentinel = reinterpret_cast<void*>(0x1300);
int g_fake_hero_id = 7;
int g_recorded_hero_id = -1;
int g_get_hero_by_id_calls = 0;
std::string g_recorded_hero_name;
int g_hero_id_pointer_count = 0;

// ManagerGetter stands in for the TLS-guarded CHeroDefinitionManager getter
// decoded from the recorded anchor.
void* ManagerGetter() {
  return g_manager_sentinel;
}

// RecordHeroNameToId mirrors the native ABI: HeroNameToId returns a
// pointer and the lookup only reads *out_id, never the returned pointer.
int* RecordHeroNameToId(void* manager, int* out_id, const char* name) {
  // Record the lookup and answer the fake id.
  EXPECT_EQ(manager, g_manager_sentinel);
  *out_id = g_fake_hero_id;
  g_recorded_hero_name = name;
  ++g_hero_id_pointer_count;
  return nullptr;
}

void* RecordGetHeroById(void* manager, unsigned hero_id) {
  EXPECT_EQ(manager, g_manager_sentinel);
  ++g_get_hero_by_id_calls;
  g_recorded_hero_id = static_cast<int>(hero_id);
  return g_hero_def_sentinel;
}

HeroDefinitionCalls RecordingCalls() {
  // Point each call at its recorder.
  HeroDefinitionCalls calls;
  calls.manager_getter = &ManagerGetter;
  calls.hero_name_to_id = &RecordHeroNameToId;
  calls.get_hero_by_id = &RecordGetHeroById;
  return calls;
}

void ResetFakes() {
  // Reset every recorded value.
  g_manager_sentinel = reinterpret_cast<void*>(0x1400);
  g_fake_hero_id = 7;
  g_recorded_hero_id = -1;
  g_get_hero_by_id_calls = 0;
  g_recorded_hero_name.clear();
  g_hero_id_pointer_count = 0;
}

class HeroDefinitionsTest : public ::testing::Test {
 public:
  HeroDefinitionsTest() : owner_(RecordingCalls()) {}

 protected:
  void SetUp() override { ResetFakes(); }

  HeroDefinitions owner_;
};

// ---- Resolve: the recorded signatures must land in a synthetic image laid
// from GameSignatures() itself, mirroring how a live session scans
// server.dll. ----

constexpr std::string_view kSignatureIds[] = {
    "hero-definition-manager.get-manager",
    "hero-definition-manager.hero-name-to-id",
    "hero-definition-manager.get-hero-by-id",
};

gti::FakeModuleImage ImageWithout(std::string_view missing) {
  // Add every signature but the missing one, and patch the manager call.
  gti::FakeModuleImage image;
  for (const auto id : kSignatureIds) {
    if (id != missing) {
      image.AddSignature(id);
    }
  }
  image.Seal(reinterpret_cast<std::uintptr_t>(&ManagerGetter));
  image.PatchRelativeCall("hero-definition-manager.get-manager", 0xE,
                          reinterpret_cast<void*>(&ManagerGetter));
  return image;
}

TEST(HeroDefinitionsResolveTest, ResolveAcceptsCompleteImage) {
  const auto image = ImageWithout({});
  auto owner = HeroDefinitions::Resolve(image);
  ASSERT_TRUE(owner.has_value()) << owner.error();
}

TEST(HeroDefinitionsResolveTest, ResolveFailsClosedWhenASignatureIsAbsent) {
  const auto image = ImageWithout("hero-definition-manager.hero-name-to-id");
  auto owner = HeroDefinitions::Resolve(image);
  ASSERT_FALSE(owner.has_value());
  EXPECT_NE(owner.error().find("hero-definition-manager.hero-name-to-id"), std::string::npos)
      << owner.error();
}

// ---- Find: the lookup contract over recording fakes. ----

TEST_F(HeroDefinitionsTest, FindResolvesNameToIdAndDefinition) {
  // A name resolves to its id and definition.
  auto hero = owner_.Find("vesta");
  ASSERT_TRUE(hero.has_value()) << hero.error();
  EXPECT_EQ(hero->id, 7);
  EXPECT_EQ(hero->native_definition_pointer, g_hero_def_sentinel);
  EXPECT_EQ(g_recorded_hero_name, "vesta");
  EXPECT_EQ(g_recorded_hero_id, 7);
}

TEST_F(HeroDefinitionsTest, FindResolvesSourceIdToDefinition) {
  // A source id resolves to its definition without a name lookup.
  auto hero = owner_.Find(17u);
  ASSERT_TRUE(hero.has_value()) << hero.error();
  EXPECT_EQ(hero->id, 17);
  EXPECT_EQ(hero->native_definition_pointer, g_hero_def_sentinel);
  EXPECT_TRUE(g_recorded_hero_name.empty());
  EXPECT_EQ(g_recorded_hero_id, 17);
}

TEST_F(HeroDefinitionsTest, FindPassesTheExactNameThrough) {
  auto hero = owner_.Find("hero_inferno");
  ASSERT_TRUE(hero.has_value()) << hero.error();
  EXPECT_EQ(g_recorded_hero_name, "hero_inferno");
}

TEST_F(HeroDefinitionsTest, FindIgnoresTheReturnedPointerAndJudgesFromOutId) {
  auto hero = owner_.Find("vesta");
  ASSERT_TRUE(hero.has_value()) << hero.error();
  // A null return pointer is not a failure when the output id resolves.
  EXPECT_EQ(g_hero_id_pointer_count, 1);
  EXPECT_EQ(hero->id, 7);
}

TEST_F(HeroDefinitionsTest, UnknownNameRefusesWithoutDefinitionRead) {
  // An unknown name stops before reading a definition.
  g_fake_hero_id = -1;
  auto refused = owner_.Find("hero_inferno");
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("unknown hero name"), std::string::npos) << refused.error();
  EXPECT_EQ(g_get_hero_by_id_calls, 0);
}

TEST_F(HeroDefinitionsTest, UnliveManagerRefusesBeforeAnyLookup) {
  // A manager that is not live stops every lookup.
  g_manager_sentinel = nullptr;
  auto refused = owner_.Find("vesta");
  g_manager_sentinel = reinterpret_cast<void*>(0x1400);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("not live yet"), std::string::npos) << refused.error();
  EXPECT_EQ(g_get_hero_by_id_calls, 0);
}

TEST_F(HeroDefinitionsTest, UnresolvedSlotRefusesBeforeAnyEngineCall) {
  // A table of unresolved calls refuses before calling the engine.
  HeroDefinitionCalls calls;  // every slot null
  modlock::gameinterop::HeroDefinitions owner(calls);
  auto refused = owner.Find("vesta");
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("unresolved"), std::string::npos) << refused.error();
}

}  // namespace
