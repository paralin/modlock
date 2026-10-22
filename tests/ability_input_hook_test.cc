#include "modlock/gameinterop/ability_input_hook.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace modlock::gameinterop {
namespace {

TEST(AbilityInputHook, ConsumesOnlySelectedBitsInEveryNativeButtonState) {
  // Lay out a pawn whose movement services hold three button states.
  std::array<unsigned char, 32> pawn{};
  std::array<unsigned char, 48> movement{};
  const uint32_t controller = (17u << 15) | 3;
  void* services = movement.data();
  constexpr uint64_t item = 0x2000000000;
  const std::array<uint64_t, 3> buttons = {item | 2 | 4, item | 2, item | 0x20};

  // Point the pawn at its controller and services, and fill the states.
  std::memcpy(pawn.data() + 4, &controller, sizeof(controller));
  std::memcpy(pawn.data() + 16, &services, sizeof(services));
  std::memcpy(movement.data() + 8, buttons.data(), sizeof(buttons));

  // Read only from the pawn and its movement services.
  const auto read = [&](const void* source, void* target, size_t size) {
    const auto address = reinterpret_cast<uintptr_t>(source);
    for (const auto region :
         {std::pair{pawn.data(), pawn.size()}, std::pair{movement.data(), movement.size()}}) {
      const auto begin = reinterpret_cast<uintptr_t>(region.first);
      if (address >= begin && size <= region.second && address - begin <= region.second - size) {
        std::memcpy(target, source, size);
        return true;
      }
    }
    return false;
  };

  // Consume the item bit from every state.
  bool called = false;
  const auto result = AbilityInputHook::Process(
      pawn.data(), {4, 16, 8}, read,
      [&](void* target, const void* source, size_t size) {
        EXPECT_EQ(target, movement.data() + 8);
        EXPECT_EQ(size, sizeof(buttons));
        std::memcpy(target, source, size);
        return true;
      },
      [&](const AbilityInputHook::Input& input) {
        // The handler sees the player's slot, controller and buttons.
        called = true;
        EXPECT_EQ(input.slot, 2);
        EXPECT_EQ(input.controller_handle, controller);
        EXPECT_EQ(input.buttons, buttons);
        return item;
      });

  // Only the item bit is cleared.
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(called);
  std::array<uint64_t, 3> remaining{};
  std::memcpy(remaining.data(), movement.data() + 8, sizeof(remaining));
  EXPECT_EQ(remaining, (std::array<uint64_t, 3>{6, 2, 0x20}));

  // A remap must be written even when it consumes no additional buttons.
  ASSERT_TRUE(AbilityInputHook::Process(
      pawn.data(), {4, 16, 8}, read,
      [&](void* target, const void* source, size_t size) {
        std::memcpy(target, source, size);
        return true;
      },
      [](AbilityInputHook::Input& input) {
        for (auto& state : input.buttons) {
          if (state & 2) {
            state = (state & ~uint64_t{2}) | (uint64_t{1} << 33);
          }
        }
        return uint64_t{0};
      }));
  std::memcpy(remaining.data(), movement.data() + 8, sizeof(remaining));
  EXPECT_EQ(remaining, (std::array<uint64_t, 3>{(uint64_t{1} << 33) | 4, uint64_t{1} << 33, 0x20}));

  // A pawn without a controller is skipped.
  called = false;
  const uint32_t no_controller = UINT32_MAX;
  std::memcpy(pawn.data() + 4, &no_controller, sizeof(no_controller));
  EXPECT_TRUE(AbilityInputHook::Process(
      pawn.data(), {4, 16, 8}, read,
      [](void*, const void*, size_t) {
        ADD_FAILURE() << "non-player input must not be written";
        return false;
      },
      [&](const AbilityInputHook::Input&) {
        called = true;
        return item;
      }));
  EXPECT_FALSE(called);
}

}  // namespace
}  // namespace modlock::gameinterop
