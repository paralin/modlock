#include "modlock/gameinterop/ability_definitions.h"

#include <array>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"

namespace {
using modlock::gameinterop::AbilityDefinitions;
std::array<unsigned char, 0x30> definition;
uint32_t requested_id;
void* Lookup(int32_t scope, uint32_t id) {
  EXPECT_EQ(scope, 4);
  requested_id = id;
  return id == 17 ? nullptr : definition.data();
}

TEST(AbilityDefinitions, ResolvesUnsignedIdentityAndPreservesDisabledState) {
  // Lay out a definition with a scope and a name.
  definition.fill(0);
  const int32_t scope = 4;
  const char* name = "upgrade_phantom_strike";
  std::memcpy(definition.data() + 8, &scope, sizeof(scope));
  std::memcpy(definition.data() + 16, &name, sizeof(name));

  // An unsigned id resolves to its definition.
  AbilityDefinitions definitions(Lookup);
  auto result = definitions.Find(0xf1234567);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(requested_id, 0xf1234567);
  EXPECT_EQ(result->name, name);
  EXPECT_EQ(result->native_definition_pointer, definition.data());
  EXPECT_FALSE(result->disabled);

  // A disabled flag shows on the next find.
  definition[0x2a] = 1;
  result = definitions.Find(0xf1234567);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->disabled);

  // Unknown ids are refused.
  EXPECT_FALSE(definitions.Find(17));
  EXPECT_FALSE(definitions.Find(0));

  // Another scope is refused.
  definition[8] = 2;
  EXPECT_FALSE(definitions.Find(123));

  // A name that is not an identifier is refused.
  definition[8] = 4;
  name = "upgrade_item; quit";
  std::memcpy(definition.data() + 16, &name, sizeof(name));
  EXPECT_FALSE(definitions.Find(123));
}

class Image : public modlock::gameinterop::ModuleImage {
 public:
  std::vector<uint8_t> bytes;
  std::uintptr_t base() const override { return reinterpret_cast<std::uintptr_t>(bytes.data()); }
  std::span<const uint8_t> image_bytes() const override { return bytes; }
};

TEST(AbilityDefinitions, RequiresAUniqueLookupSignature) {
  // An empty image has no lookup.
  Image image;
  EXPECT_FALSE(AbilityDefinitions::Resolve(image));

  // Recorded Windows lookup prologue; stack offsets are wildcarded.
  const std::vector<uint8_t> prologue{0x40, 0x53, 0x48, 0x83, 0xec, 0x20,
                                      0x89, 0x54, 0x24, 0x38, 0x8b, 0xd9};
  image.bytes = prologue;
  EXPECT_TRUE(AbilityDefinitions::Resolve(image));

  // A second copy makes the signature ambiguous.
  image.bytes.insert(image.bytes.end(), prologue.begin(), prologue.end());
  EXPECT_FALSE(AbilityDefinitions::Resolve(image));
}
}  // namespace
