#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "modlock/gameinterop/virtual_slots.h"

namespace {

using modlock::gameinterop::FindVtable;
using modlock::gameinterop::MatchingSlots;
using modlock::gameinterop::ModuleImage;
using modlock::gameinterop::VirtualSlot;
using modlock::gameinterop::VtableEntries;

constexpr std::uintptr_t kBase = 0x180000000;
constexpr size_t kTable = 0x288;

// Image lays out a two-section PE: code at 0x100 with three functions, and
// data at 0x200 holding CThing's type descriptor, object locator, and table.
class Image final : public ModuleImage {
 public:
  Image() {
    // Write a file header with a code section and a data section.
    Put<uint32_t>(0x3C, 0x40);
    Put<uint16_t>(0x40 + 6, 2);
    Section(0, 0x100, 0x100, 0x60000020);
    Section(1, 0x200, 0x200, 0x40000040);

    // The first and third functions share their entry bytes.
    const std::array<uint8_t, 2> entries[] = {{0xAA, 0xBB}, {0xCC, 0xDD}, {0xAA, 0xBB}};
    for (size_t i = 0; i < 3; ++i) {
      std::memcpy(bytes.data() + 0x100 + i * 0x10, entries[i].data(), 2);
    }

    // Write the type name, its locator and the table.
    const char name[] = ".?AVCThing@@";
    std::memcpy(bytes.data() + 0x210, name, sizeof(name));
    const uint32_t locator[] = {1, 0, 0, 0x200};
    std::memcpy(bytes.data() + 0x240, locator, sizeof(locator));
    Put<uint64_t>(kTable - 8, kBase + 0x240);
    for (size_t i = 0; i < 3; ++i) {
      Put<uint64_t>(kTable + i * 8, kBase + 0x100 + i * 0x10);
    }
  }

  std::uintptr_t base() const override { return kBase; }
  std::span<const uint8_t> image_bytes() const override { return bytes; }

 private:
  template <typename T>
  void Put(size_t at, T value) {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
  }

  void Section(size_t index, uint32_t address, uint32_t size, uint32_t flags) {
    const size_t header = 0x40 + 24 + index * 40;
    Put(header + 8, size);
    Put(header + 12, address);
    Put(header + 36, flags);
  }

  std::array<uint8_t, 0x400> bytes{};
};

VirtualSlot Slot(std::string_view class_name, size_t index, std::string_view pattern) {
  return {.id = "test.method",
          .module = "test.dll",
          .class_name = class_name,
          .index = index,
          .pattern = pattern,
          .shape = "void CThing::Method()"};
}

TEST(VirtualSlots, FindsTableThroughTypeInformation) {
  // The table is found through its type information.
  const Image image;
  const auto table = FindVtable(image, "CThing");
  ASSERT_TRUE(table) << table.error();
  EXPECT_EQ(*table, kTable);
  EXPECT_EQ(VtableEntries(image, *table), (std::vector<size_t>{0x100, 0x110, 0x120}));
}

TEST(VirtualSlots, MissingClassIsAnError) {
  const auto table = FindVtable(Image{}, "CMissing");
  ASSERT_FALSE(table);
  EXPECT_EQ(table.error(), "class CMissing type descriptor not found");
}

TEST(VirtualSlots, ReportsEveryMatchingIndex) {
  // A unique pattern matches one index.
  const Image image;
  const auto unique = MatchingSlots(image, kTable, Slot("CThing", 1, "CC DD"));
  ASSERT_TRUE(unique) << unique.error();
  EXPECT_EQ(*unique, (std::vector<size_t>{1}));

  // A shared one matches each.
  const auto shared = MatchingSlots(image, kTable, Slot("CThing", 0, "AA ??"));
  ASSERT_TRUE(shared) << shared.error();
  EXPECT_EQ(*shared, (std::vector<size_t>{0, 2}));
}

}  // namespace
