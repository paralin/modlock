// Contract test for the resource-service read: the entity system lives at
// the live-build offset 0x58 (vtable-accessor proof in entity_abi.h); the
// stale mirror offset 0x38 must never be selected.
#include <array>
#include <cstring>

#include "gtest/gtest.h"
#include "modlock/gameinterop/entity_abi.h"

namespace {

TEST(EntitySystemFromResourceService, ReadsTheLiveOffsetNotTheStaleMirrorOne) {
  // Independently encoded service window: the stale-mirror slot holds the
  // exact garbage the host10 crash dereferenced; the live slot holds the
  // entity system.
  constexpr size_t kStaleMirrorOffset = 0x38;
  constexpr size_t kLiveOffset = 0x58;
  std::array<unsigned char, 0x80> service{};
  void* stale = reinterpret_cast<void*>(0x100);
  void* entity_system = reinterpret_cast<void*>(0x464E77EE000ULL);
  std::memcpy(service.data() + kStaleMirrorOffset, &stale, sizeof(stale));
  std::memcpy(service.data() + kLiveOffset, &entity_system, sizeof(entity_system));

  // The live offset resolves.
  auto resolved = modlock::gameinterop::EntitySystemFromResourceService(service.data());
  ASSERT_TRUE(resolved.has_value()) << resolved.error();
  EXPECT_EQ(resolved.value(), entity_system);
}

TEST(EntitySystemFromResourceService, RejectsAnUnresolvedService) {
  auto resolved = modlock::gameinterop::EntitySystemFromResourceService(nullptr);
  ASSERT_FALSE(resolved.has_value());
  EXPECT_FALSE(resolved.error().empty());
}

}  // namespace
