#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"

namespace modlock::test_support {

// FakeEntityList lays out one entity in the native entity list: the system's
// first chunk holds its identity, which points back at the entity and carries
// a live handle, so EntityInstance and EntityRef resolve it.
struct FakeEntityList {
  static constexpr uint32_t kHandle = 0x18000;

  alignas(16) std::array<void*, 4> system{};
  alignas(16) std::array<std::byte, 0x70> identity{};

  // Link makes entity the live occupant of the list's first slot.
  void Link(void* entity) {
    identity.fill({});
    system.fill(nullptr);
    system[2] = identity.data();
    Write(identity.data(), entity);
    Write(identity.data() + gameinterop::kIdentityEHandleOffset, kHandle);
    Write(static_cast<std::byte*>(entity) + gameinterop::kIdentityOffset, identity.data());
  }

  // Delete frees the slot, as the engine does when it removes the entity.
  void Delete() {
    Write(identity.data() + gameinterop::kIdentityEHandleOffset, gameinterop::kInvalidEHandleIndex);
  }

  void* System() { return system.data(); }

 private:
  template <typename T>
  static void Write(std::byte* at, const T& value) {
    std::memcpy(at, &value, sizeof(value));
  }
};

}  // namespace modlock::test_support
