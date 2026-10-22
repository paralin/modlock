#include "modlock/gameinterop/engine_server.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include "gtest/gtest.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace {
using modlock::gameinterop::EngineServer;
using modlock::gameinterop::FindVirtualSlot;

const size_t kCommand = FindVirtualSlot("engine-server.server-command")->index;
const size_t kGlobals = FindVirtualSlot("engine-server.globals")->index;

struct Fixture {
  void** table = nullptr;
  std::vector<void*> functions = std::vector<void*>(std::max(kCommand, kGlobals) + 1);
  std::array<unsigned char, 96> globals{};
  bool available = true;
  Fixture() {
    table = functions.data();
    functions[kCommand] = reinterpret_cast<void*>(+[](void*, const char*) {});
    functions[kGlobals] = reinterpret_cast<void*>(+[](void* self) -> const void* {
      auto* fixture = static_cast<Fixture*>(self);
      return fixture->available ? fixture->globals.data() : nullptr;
    });
  }
  void Set(float time, int32_t tick, float interval) {
    std::memcpy(globals.data() + 48, &time, 4);
    std::memcpy(globals.data() + 68, &tick, 4);
    std::memcpy(globals.data() + 84, &interval, 4);
  }
};

TEST(EngineServer, ReadsCopiedSimulationClockAndTracksPauseAndAdvance) {
  // Bind to the fixture.
  Fixture fixture;
  auto engine = EngineServer::Bind(&fixture);
  ASSERT_TRUE(engine);

  // Read the copied clock.
  fixture.Set(10, 640, 0.015625f);
  auto clock = engine->ReadClock();
  ASSERT_TRUE(clock) << clock.error();
  EXPECT_EQ(clock->current_time, 10);
  EXPECT_EQ(clock->tick, 640);
  EXPECT_EQ(clock->interval, 0.015625f);

  // A paused clock reads the same time.
  auto paused = engine->ReadClock();
  ASSERT_TRUE(paused);
  EXPECT_EQ(paused->current_time, clock->current_time);

  // An advance moves the tick and leaves the earlier copy.
  fixture.Set(10.015625f, 641, 0.015625f);
  auto advanced = engine->ReadClock();
  ASSERT_TRUE(advanced);
  EXPECT_EQ(advanced->tick, 641);
  EXPECT_EQ(clock->tick, 640);

  // An unavailable engine or missing globals read nothing.
  fixture.available = false;
  EXPECT_FALSE(engine->ReadClock());
  fixture.available = true;
  fixture.functions[kGlobals] = nullptr;
  EXPECT_FALSE(engine->ReadClock());

  // Binding needs the engine and its command call.
  EXPECT_FALSE(EngineServer::Bind(nullptr));
  fixture.functions[kCommand] = nullptr;
  EXPECT_FALSE(EngineServer::Bind(&fixture));
}

TEST(EngineServer, RefusesMalformedOrInconsistentGlobals) {
  // Bind to the fixture.
  Fixture fixture;
  auto engine = EngineServer::Bind(&fixture);
  ASSERT_TRUE(engine);

  // Each malformed clock is refused.
  for (const auto& bad : std::array<EngineServer::SimulationClock, 5>{
           {{std::numeric_limits<float>::quiet_NaN(), 1, 0.015625f},
            {1, -1, 0.015625f},
            {1, 64, 0},
            {1, 64, std::numeric_limits<float>::infinity()},
            {1000, 64, 0.015625f}}}) {
    fixture.Set(bad.current_time, bad.tick, bad.interval);
    EXPECT_FALSE(engine->ReadClock());
  }

  // A valid clock reads.
  fixture.Set(0, 0, 0.015625f);
  EXPECT_TRUE(engine->ReadClock());
}
}  // namespace
