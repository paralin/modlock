#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace {

using modlock::gameinterop::FindVirtualSlot;

const size_t kTeleport = FindVirtualSlot("entity.teleport")->index;

// FakeTeleport records the exact arguments the CBaseEntity::Teleport vtable
// call receives: three float[3] array pointers (position, angles, velocity).
struct FakeTeleport {
  float position[3];
  float angles[3];
  float velocity[3];
  int calls = 0;
};

// FakeEntity carries the vptr every MSVC object leads with; the entity.teleport
// slot of the table is the engine's own CBaseEntity::Teleport.
struct FakeEntity {
  void* vtable;
  FakeTeleport teleport;
};

void RecordTeleport(void* self, const float* position, const float* angles, const float* velocity) {
  // Record the call's arguments on the entity.
  auto* entity = static_cast<FakeEntity*>(self);
  std::memcpy(entity->teleport.position, position, sizeof(entity->teleport.position));
  std::memcpy(entity->teleport.angles, angles, sizeof(entity->teleport.angles));
  if (velocity) {
    std::memcpy(entity->teleport.velocity, velocity, sizeof(entity->teleport.velocity));
  }
  entity->teleport.calls++;
}

TEST(TeleportEntity, DispatchesTheExactFloatTripletOnTheTeleportSlot) {
  // Point the teleport slot at the recorder.
  std::vector<void*> table(kTeleport + 1);
  table[kTeleport] = reinterpret_cast<void*>(&RecordTeleport);

  // Teleport to a position alone.
  FakeEntity entity{table.data(), {}};
  modlock::gameinterop::TeleportEntity(&entity, 12.5f, -3.25f, 1024.0f);

  // The position arrives exactly, with zero angles.
  ASSERT_EQ(entity.teleport.calls, 1);
  EXPECT_FLOAT_EQ(entity.teleport.position[0], 12.5f);
  EXPECT_FLOAT_EQ(entity.teleport.position[1], -3.25f);
  EXPECT_FLOAT_EQ(entity.teleport.position[2], 1024.0f);
  EXPECT_FLOAT_EQ(entity.teleport.angles[0], 0.0f);
  EXPECT_FLOAT_EQ(entity.teleport.angles[1], 0.0f);
  EXPECT_FLOAT_EQ(entity.teleport.angles[2], 0.0f);

  // The velocity is zero.
  EXPECT_FLOAT_EQ(entity.teleport.velocity[0], 0.0f);
  EXPECT_FLOAT_EQ(entity.teleport.velocity[1], 0.0f);
  EXPECT_FLOAT_EQ(entity.teleport.velocity[2], 0.0f);
}

TEST(TeleportEntity, IgnoresANullEntity) {
  modlock::gameinterop::TeleportEntity(nullptr, 1.0f, 2.0f, 3.0f);
}

TEST(TeleportEntity, PreservesReplayOrientationAndVelocity) {
  // Teleport with a full orientation and velocity.
  std::vector<void*> table(kTeleport + 1);
  table[kTeleport] = reinterpret_cast<void*>(&RecordTeleport);
  FakeEntity entity{table.data(), {}};
  const std::array<float, 3> position{10.5f, -20.25f, 704.0f};
  const std::array<float, 3> angles{7.5f, 120.0f, -3.0f};
  const std::array<float, 3> velocity{250.0f, -110.0f, 80.0f};
  modlock::gameinterop::TeleportEntity(&entity, position, angles, velocity);

  // Every component arrives exactly.
  ASSERT_EQ(entity.teleport.calls, 1);
  for (size_t i = 0; i != 3; ++i) {
    EXPECT_FLOAT_EQ(entity.teleport.position[i], position[i]);
    EXPECT_FLOAT_EQ(entity.teleport.angles[i], angles[i]);
    EXPECT_FLOAT_EQ(entity.teleport.velocity[i], velocity[i]);
  }
}

TEST(TeleportEntity, EmptyVelocityStopsAMovingPawn) {
  // Set up an entity that is already moving.
  std::vector<void*> table(kTeleport + 1);
  table[kTeleport] = reinterpret_cast<void*>(&RecordTeleport);
  FakeEntity entity{table.data(), {}};
  entity.teleport.velocity[0] = 800.0f;
  entity.teleport.velocity[1] = -600.0f;
  entity.teleport.velocity[2] = 200.0f;

  // Teleport with no velocity.
  modlock::gameinterop::TeleportEntity(&entity, {7612.0f, 240.0f, 120.0f}, {0.0f, 270.0f, 0.0f},
                                       {});

  // The entity stops.
  ASSERT_EQ(entity.teleport.calls, 1);
  for (float component : entity.teleport.velocity) {
    EXPECT_FLOAT_EQ(component, 0.0f);
  }
}

}  // namespace
