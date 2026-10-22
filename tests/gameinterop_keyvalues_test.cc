// Keyvalues contracts cover unique symbol resolution, engine member-name
// hashing, and typed construction through the native ABI.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "gameinterop_module_image_test.h"
#include "gtest/gtest.h"
#include "modlock/gameinterop/keyvalues.h"

namespace {

using modlock::gameinterop::BuildEntityKeyValues;
using modlock::gameinterop::EntityKeyValue;
using modlock::gameinterop::KeyValuesCalls;
using modlock::gameinterop::MakeMemberName;
using modlock::gameinterop::ResolveKeyValuesCalls;
namespace gti = modlock::gameinterop::testing;

constexpr std::string_view kSignatureIds[] = {
    "entity-keyvalues.allocate",
    "entity-keyvalues.construct",
    "entity-keyvalues.set-key-value",
};

// BuildImage lays every recorded keyvalues signature except missing.
gti::FakeModuleImage BuildImage(std::string_view missing = {}) {
  gti::FakeModuleImage image;
  for (const auto id : kSignatureIds) {
    if (id != missing) {
      image.AddSignature(id);
    }
  }
  image.Seal(0x50000000);
  return image;
}

TEST(KeyValuesTest, ResolveFillsEverySlotFromASyntheticImage) {
  // Each call resolves to its signature's offset.
  const auto image = BuildImage();
  auto resolved = ResolveKeyValuesCalls(image);
  ASSERT_TRUE(resolved.has_value()) << resolved.error();
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(resolved->allocate),
            image.base() + image.OffsetOf("entity-keyvalues.allocate"));
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(resolved->construct_key_values),
            image.base() + image.OffsetOf("entity-keyvalues.construct"));
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(resolved->set_key_value),
            image.base() + image.OffsetOf("entity-keyvalues.set-key-value"));
}

TEST(KeyValuesTest, AllocationResolvesUpdatedBuildAmongSimilarWrappers) {
  // Build an image holding two similar allocator wrappers.
  gti::FakeModuleImage image;

  // Recorded server.dll build 25173285 bytes. The first wrapper is a
  // separate allocator entry; the second is followed by the realloc wrapper.
  image.Add("other-allocator", "48 8B 05 61 1F 86 00 48 8B D1 48 8B 08 48 8B 01 48 FF 60 08");
  image.Add("allocator",
            "48 8B 05 31 1C 86 00 48 8B D1 48 8B 08 48 8B 01 48 FF 60 08 "
            "CC CC CC CC CC CC CC CC CC CC CC CC "
            "48 8B 05 11 1C 86 00 4C 8B C9 4C 8B C2 49 8B D1");
  image.AddSignature("entity-keyvalues.construct");
  image.AddSignature("entity-keyvalues.set-key-value");
  image.Seal(0x50000000);

  // The allocator with the realloc wrapper after it is chosen.
  auto resolved = ResolveKeyValuesCalls(image);
  ASSERT_TRUE(resolved.has_value()) << resolved.error();
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(resolved->allocate),
            image.base() + image.OffsetOf("allocator"));
}

TEST(KeyValuesTest, AllocationRefusesDuplicateMatches) {
  // A second allocator match is refused.
  auto image = BuildImage();
  image.AddSignature("entity-keyvalues.allocate");
  image.Seal(0x50000000);
  auto resolved = ResolveKeyValuesCalls(image);
  ASSERT_FALSE(resolved.has_value());
  EXPECT_NE(resolved.error().find("entity-keyvalues.allocate' matched 2 times"), std::string::npos);
}

TEST(KeyValuesTest, ResolveNamesASignatureMissingFromTheImage) {
  const auto image = BuildImage("entity-keyvalues.set-key-value");
  auto resolved = ResolveKeyValuesCalls(image);
  ASSERT_FALSE(resolved.has_value());
  EXPECT_NE(resolved.error().find("entity-keyvalues.set-key-value"), std::string::npos)
      << resolved.error();
}

TEST(KeyValuesTest, MemberNameTranscribesTheStringTokenHash) {
  // MurmurHash2 over the lowercased key with STRINGTOKEN_MURMURHASH_SEED.
  const auto name = MakeMemberName("Font_Name");
  EXPECT_EQ(name.hash, 0x7a0c84aau);
  EXPECT_EQ(name.symbol, 0xFFFFFFFFu);
  EXPECT_EQ(std::string_view(name.string), "Font_Name");

  // Case-insensitive: the hash folds case even though the pointer keeps the
  // caller's spelling.
  EXPECT_EQ(MakeMemberName("FONT_NAME").hash, name.hash);
}

struct RecordedSetterCall {
  std::string key;
  std::uint32_t hash;
};

void* g_created_key_values = reinterpret_cast<void*>(0x2000);
std::vector<std::string> g_setter_order;
RecordedSetterCall g_last_call;

void* RecordCreate() {
  return g_created_key_values;
}

void RecordSetString(void* ekv, const modlock::gameinterop::MemberName* name, const char* value) {
  EXPECT_EQ(ekv, g_created_key_values);
  g_setter_order.push_back("set-string:" + std::string(value));
  g_last_call = {name->string, name->hash};
}

void RecordSetBool(void* ekv, const modlock::gameinterop::MemberName* name, unsigned char value) {
  EXPECT_EQ(ekv, g_created_key_values);
  g_setter_order.push_back("set-bool:" + std::to_string(value));
  g_last_call = {name->string, name->hash};
}

void RecordSetInt(void* ekv, const modlock::gameinterop::MemberName* name, int value) {
  EXPECT_EQ(ekv, g_created_key_values);
  g_setter_order.push_back("set-int:" + std::to_string(value));
  g_last_call = {name->string, name->hash};
}

void RecordSetColor(void* ekv, const modlock::gameinterop::MemberName* name, std::uint32_t packed) {
  EXPECT_EQ(ekv, g_created_key_values);
  g_setter_order.push_back("set-color:" + std::to_string(packed));
  g_last_call = {name->string, name->hash};
}

TEST(KeyValuesTest, BuildAppliesEveryTypedPairThroughItsSetter) {
  // Record the factory and each setter.
  KeyValuesCalls calls;
  calls.create_key_values = &RecordCreate;
  calls.set_string = &RecordSetString;
  calls.set_bool = &RecordSetBool;
  calls.set_int = &RecordSetInt;
  calls.set_color = &RecordSetColor;

  // Build one pair of each kind.
  const std::vector<EntityKeyValue> pairs = {
      {.key = "message_text", .value = std::string_view("hello")},
      {.key = "enabled", .value = true},
      {.key = "font_size", .value = 100},
      {.key = "color",
       .value = modlock::gameinterop::KeyValueColor{.red = 1, .green = 2, .blue = 3, .alpha = 4}},
  };

  // Each setter ran once, in order.
  auto built = BuildEntityKeyValues(calls, pairs);
  ASSERT_TRUE(built.has_value()) << built.error();
  EXPECT_EQ(*built, g_created_key_values);
  ASSERT_EQ(g_setter_order.size(), 4u);
  EXPECT_EQ(g_setter_order[0], "set-string:hello");
  EXPECT_EQ(g_setter_order[1], "set-bool:1");
  EXPECT_EQ(g_setter_order[2], "set-int:100");

  // Packed little-endian RGBA word: r | g << 8 | b << 16 | a << 24.
  EXPECT_EQ(g_setter_order[3], "set-color:" + std::to_string(0x04030201u));
  EXPECT_EQ(g_last_call.key, "color");
  EXPECT_EQ(g_last_call.hash, MakeMemberName("color").hash);
}

struct RawMember {
  std::uint64_t metadata = 1ull << 2;
  std::uint64_t data = 0;
};

std::array<std::byte, 56> g_raw_ekv;
std::array<RawMember, 8> g_raw_members;
size_t g_raw_member_count = 0;

void* RecordAllocate(size_t size) {
  EXPECT_EQ(size, g_raw_ekv.size());
  g_raw_ekv.fill(std::byte{});
  return g_raw_ekv.data();
}

void* RecordConstruct(void* storage, void* allocator, unsigned char allocator_type) {
  EXPECT_EQ(storage, g_raw_ekv.data());
  EXPECT_EQ(allocator, nullptr);
  EXPECT_EQ(allocator_type, 0);
  return storage;
}

void* RecordSetKeyValue(void* ekv, const modlock::gameinterop::MemberName*,
                        unsigned char as_attribute) {
  // Hand out the next fresh member.
  EXPECT_EQ(ekv, g_raw_ekv.data());
  EXPECT_EQ(as_attribute, 0);
  EXPECT_LT(g_raw_member_count, g_raw_members.size());
  g_raw_members[g_raw_member_count] = RawMember{};
  return &g_raw_members[g_raw_member_count++];
}

TEST(KeyValuesTest, NativeSurfaceEncodesFreshMembersExactly) {
  // Record the native fresh member surface.
  g_raw_member_count = 0;
  KeyValuesCalls calls;
  calls.allocate = &RecordAllocate;
  calls.construct_key_values = &RecordConstruct;
  calls.set_key_value = &RecordSetKeyValue;

  // Build one pair of each native kind.
  const std::string message = "CURRENT 1/18";
  const std::vector<EntityKeyValue> pairs = {
      {.key = "message_text", .value = std::string_view(message)},
      {.key = "enabled", .value = true},
      {.key = "fullbright", .value = 1},
      {.key = "font_size", .value = 100.0f},
      {.key = "color",
       .value = modlock::gameinterop::KeyValueColor{.red = 1, .green = 2, .blue = 3, .alpha = 255}},
  };

  // Five members were written.
  auto built = BuildEntityKeyValues(calls, pairs);
  ASSERT_TRUE(built.has_value()) << built.error();
  EXPECT_EQ(*built, g_raw_ekv.data());
  ASSERT_EQ(g_raw_member_count, 5u);

  // Read each member's type and subtype from its metadata.
  auto type = [](const RawMember& value) {
    return (value.metadata >> 2) & 0xFF;
  };
  auto subtype = [](const RawMember& value) {
    return (value.metadata >> 10) & 0xFF;
  };

  // Each member carries its kind's type and data.
  EXPECT_EQ(type(g_raw_members[0]), 0x26u);
  EXPECT_EQ(subtype(g_raw_members[0]), 26u);
  EXPECT_EQ(g_raw_members[0].data, reinterpret_cast<std::uintptr_t>(message.c_str()));
  EXPECT_EQ(type(g_raw_members[1]), 2u);
  EXPECT_EQ(g_raw_members[1].data, 1u);
  EXPECT_EQ(type(g_raw_members[2]), 3u);
  EXPECT_EQ(g_raw_members[2].data, 1u);
  EXPECT_EQ(type(g_raw_members[3]), 5u);

  // The float widens to a double.
  double font_size = 0;
  std::memcpy(&font_size, &g_raw_members[3].data, sizeof(font_size));
  EXPECT_EQ(font_size, 100.0);

  // The color packs its four channels.
  EXPECT_EQ(type(g_raw_members[4]), 0x88u);
  EXPECT_EQ(subtype(g_raw_members[4]), 28u);
  EXPECT_EQ((g_raw_members[4].metadata >> 34) & 0x1F, 3u);
  EXPECT_EQ(g_raw_members[4].data, 0xFF030201u);
}

TEST(KeyValuesTest, NativeVectorBorrowsAllComponentsThroughQueuedCreation) {
  // Record the native fresh member surface.
  g_raw_member_count = 0;
  KeyValuesCalls calls;
  calls.allocate = &RecordAllocate;
  calls.construct_key_values = &RecordConstruct;
  calls.set_key_value = &RecordSetKeyValue;

  // Build one vector.
  const EntityKeyValue pairs[] = {
      {.key = "scales", .value = modlock::gameinterop::KeyValueVector{0.22f, 0.3f, 0.4f}}};
  auto built = BuildEntityKeyValues(calls, pairs);
  ASSERT_TRUE(built.has_value()) << built.error();
  ASSERT_EQ(g_raw_member_count, 1u);

  // The vector's metadata names three floats.
  const auto& member = g_raw_members[0];
  EXPECT_EQ((member.metadata >> 2) & 0xFF, 0x48u);
  EXPECT_EQ((member.metadata >> 10) & 0xFF, 24u);
  EXPECT_EQ((member.metadata >> 34) & 0x1F, 3u);
  EXPECT_EQ(member.metadata & (1ull << 1), 0u);

  // Its data points at the caller's vector, which stays readable.
  const auto& vector = std::get<modlock::gameinterop::KeyValueVector>(pairs[0].value);
  EXPECT_EQ(member.data, reinterpret_cast<std::uintptr_t>(&vector.x));
  std::array<float, 3> consumed{};
  std::memcpy(consumed.data(), reinterpret_cast<const void*>(member.data), sizeof(consumed));
  EXPECT_EQ(consumed, (std::array<float, 3>{0.22f, 0.3f, 0.4f}));
}

// Build 6723's KeyValues3 metadata has no flags byte: the cluster element is
// bits 18-33 and the array count bits 34-38 (the engine reads shr 0x22 & 0x1f).
// The count must land there, and the member's cluster element must survive.
TEST(KeyValuesTest, ArrayCountUsesTheBuild6723MetadataLayout) {
  // Record the native surface, with arena bits set on each member.
  g_raw_member_count = 0;
  KeyValuesCalls calls;
  calls.allocate = &RecordAllocate;
  calls.construct_key_values = &RecordConstruct;
  calls.set_key_value = +[](void* ekv, const modlock::gameinterop::MemberName* name,
                            unsigned char as_attribute) -> void* {
    auto* member = static_cast<RawMember*>(RecordSetKeyValue(ekv, name, as_attribute));
    member->metadata |= 0xBEEFull << 18;  // cluster element assigned by the arena
    return member;
  };

  // Build a color and a vector.
  const EntityKeyValue pairs[] = {
      {.key = "rendercolor",
       .value = modlock::gameinterop::KeyValueColor{.red = 9, .green = 8, .blue = 7, .alpha = 6}},
      {.key = "scales", .value = modlock::gameinterop::KeyValueVector{1.f, 2.f, 3.f}},
  };

  // Each count lands in its field without touching the arena bits.
  auto built = BuildEntityKeyValues(calls, pairs);
  ASSERT_TRUE(built.has_value()) << built.error();
  ASSERT_EQ(g_raw_member_count, 2u);
  for (const auto& [member, count] :
       {std::pair{g_raw_members[0], 4u}, std::pair{g_raw_members[1], 3u}}) {
    EXPECT_EQ((member.metadata >> 34) & 0x1F, count);
    EXPECT_EQ((member.metadata >> 18) & 0xFFFF, 0xBEEFu);
    EXPECT_EQ(member.metadata >> 39, 0u) << "nothing may be written above the count";
  }
}

TEST(KeyValuesTest, BuildNamesTheFirstMissingConstructor) {
  auto built = BuildEntityKeyValues(KeyValuesCalls{}, {});
  ASSERT_FALSE(built.has_value());
  EXPECT_NE(built.error().find("complete native"), std::string::npos) << built.error();
}

TEST(KeyValuesTest, BuildNamesTheMissingSetterForItsKind) {
  // Record only the factory, then build a vector.
  KeyValuesCalls calls;
  calls.create_key_values = &RecordCreate;
  const std::vector<EntityKeyValue> pairs = {
      {.key = "origin", .value = modlock::gameinterop::KeyValueVector{1, 2, 3}}};
  auto built = BuildEntityKeyValues(calls, pairs);
  ASSERT_FALSE(built.has_value());
  EXPECT_NE(built.error().find("entity-keyvalues.set-vector"), std::string::npos) << built.error();
}

}  // namespace
