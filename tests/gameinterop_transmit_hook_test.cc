// Contract tests for the per-player transmit seam: reading a player's slot and
// withholding entities from the transmit bit vector the engine filled.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "modlock/gameinterop/transmit_hook.h"

namespace {

using modlock::gameinterop::TransmitHook;
using modlock::gameinterop::TransmitSet;

// FakeInfo lays out the CCheckTransmitInfo fields the seam reads: the transmit
// bit vector pointer first and the player slot at kSlotOffset.
struct FakeInfo {
  explicit FakeInfo(int32_t slot) {
    bits.fill(~uint32_t{0});
    uint32_t* pointer = bits.data();
    std::memcpy(bytes.data(), &pointer, sizeof(pointer));
    std::memcpy(bytes.data() + TransmitSet::kSlotOffset, &slot, sizeof(slot));
  }

  bool Sends(uint32_t index) const { return (bits[index / 32] >> (index % 32)) & 1; }

  std::array<uint32_t, TransmitSet::kEntityCount / 32> bits;
  alignas(void*) std::array<uint8_t, TransmitSet::kSlotOffset + 8> bytes{};
};

TEST(TransmitSet, ReadsThePlayerSlot) {
  FakeInfo info(7);
  TransmitSet set(info.bytes.data());
  EXPECT_EQ(set.Slot(), 7);
}

TEST(TransmitSet, WithholdsOnlyTheHandlesEntity) {
  FakeInfo info(0);
  TransmitSet set(info.bytes.data());

  // The serial number above the index bits does not matter.
  set.Withhold((5u << 15) | 300);
  EXPECT_FALSE(info.Sends(300));
  EXPECT_TRUE(info.Sends(299));
  EXPECT_TRUE(info.Sends(301));
}

TEST(TransmitSet, IgnoresAnIndexPastTheSet) {
  FakeInfo info(0);
  TransmitSet set(info.bytes.data());

  set.Withhold(TransmitSet::kEntityCount + 1);
  for (uint32_t word : info.bits) EXPECT_EQ(word, ~uint32_t{0});
}

TEST(TransmitHook, InstallFailsClosedWithoutMappedServerModule) {
  // No game modules are mapped in this process, so the only correct outcome
  // is a named error and no hook.
  auto hook = TransmitHook::Install([](TransmitSet&) {});
  ASSERT_FALSE(hook.has_value());
  EXPECT_FALSE(hook.error().empty());
}

}  // namespace
