#include "modlock/gameinterop/modifier_states.h"

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace {

// FakeImage lays out a module whose schema enumerator records point at their
// name strings, as the game's server module does.
struct FakeImage final : modlock::gameinterop::ModuleImage {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(256, 0);
  std::uintptr_t base() const override { return 0x180000000; }
  std::span<const uint8_t> image_bytes() const override { return bytes; }

  // Name places a NUL-terminated string at offset and returns its address.
  uint64_t Name(size_t offset, std::string_view text) {
    std::memcpy(bytes.data() + offset, text.data(), text.size());
    bytes[offset + text.size()] = 0;
    return base() + offset;
  }
  // Record places {name pointer, value} at an aligned offset.
  void Record(size_t offset, uint64_t name, int64_t value) {
    std::memcpy(bytes.data() + offset, &name, sizeof(name));
    std::memcpy(bytes.data() + offset + 8, &value, sizeof(value));
  }
};

TEST(ModifierStates, ReadsEachValueBesideItsNamePointer) {
  FakeImage image;
  const auto draw = image.Name(9, "MODIFIER_STATE_DO_NOT_DRAW_MODEL");
  const auto quiet = image.Name(60, "MODIFIER_STATE_DO_NOT_DRAW_MODEL_SHADOW");
  image.Record(128, draw, 189);
  image.Record(144, quiet, 190);
  auto value = modlock::gameinterop::ModifierStateIndex(image, "MODIFIER_STATE_DO_NOT_DRAW_MODEL");
  ASSERT_TRUE(value) << value.error();
  EXPECT_EQ(*value, 189u);

  constexpr std::array<std::string_view, 2> names = {"MODIFIER_STATE_DO_NOT_DRAW_MODEL_SHADOW",
                                                     "MODIFIER_STATE_DO_NOT_DRAW_MODEL"};
  auto both = modlock::gameinterop::ModifierStateIndices(image, names);
  ASSERT_TRUE(both) << both.error();
  EXPECT_EQ(*both, (std::vector<uint32_t>{190, 189}));
}

TEST(ModifierStates, RefusesMissingAndConflictingNames) {
  FakeImage image;
  const auto draw = image.Name(9, "MODIFIER_STATE_DO_NOT_DRAW_MODEL");
  EXPECT_FALSE(modlock::gameinterop::ModifierStateIndex(image, "MODIFIER_STATE_UNKNOWN"));
  // A string without an enumerator record is not a state.
  EXPECT_FALSE(modlock::gameinterop::ModifierStateIndex(image, "MODIFIER_STATE_DO_NOT_DRAW_MODEL"));
  image.Record(128, draw, 189);
  image.Record(160, draw, 184);
  EXPECT_FALSE(modlock::gameinterop::ModifierStateIndex(image, "MODIFIER_STATE_DO_NOT_DRAW_MODEL"));
}

}  // namespace
