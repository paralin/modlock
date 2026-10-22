// Contract tests for the connection tracker's engine callback order. The
// vtable slots follow the native interface; these tests exercise dispatch
// ordering independently of the installed game.
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "modlock/gameinterop/connection_tracker.h"

namespace {

using modlock::gameinterop::ConnectionEventSink;
using modlock::gameinterop::ConnectionTracker;

class RecordingSink final : public ConnectionEventSink {
 public:
  RecordingSink(std::vector<std::string>& events, bool* entity_live = nullptr)
      : events_(events), entity_live_(entity_live) {}

  void OnConnected(int32_t slot, uint64_t xuid, bool is_bot, const char* name) override {
    events_.push_back("connected:" + std::to_string(slot) + ":" + std::to_string(xuid) + ":" +
                      (is_bot ? "bot:" : "human:") + (name != nullptr ? name : ""));
    EXPECT_TRUE(ConnectionTracker::StateForSlot(slot).occupied);
    EXPECT_EQ(ConnectionTracker::StateForSlot(slot).xuid, xuid);
  }

  void OnDisconnecting(int32_t slot, uint64_t xuid) override {
    events_.push_back("disconnecting:" + std::to_string(slot) + ":" + std::to_string(xuid));
    EXPECT_TRUE(ConnectionTracker::StateForSlot(slot).occupied);
    EXPECT_EQ(ConnectionTracker::StateForSlot(slot).xuid, xuid);
    if (entity_live_ != nullptr) {
      EXPECT_TRUE(*entity_live_);
    }
  }

 private:
  std::vector<std::string>& events_;
  bool* entity_live_;
};

class CleanupSink final : public ConnectionEventSink {
 public:
  explicit CleanupSink(bool& entity_live) : entity_live_(entity_live) {}

  void OnConnected(int32_t /*slot*/, uint64_t /*xuid*/, bool /*is_bot*/,
                   const char* /*name*/) override {}

  void OnDisconnecting(int32_t /*slot*/, uint64_t /*xuid*/) override {
    EXPECT_TRUE(entity_live_);
    entity_live_ = false;
  }

 private:
  bool& entity_live_;
};

class CallbackSink final : public ConnectionEventSink {
 public:
  CallbackSink(int& connected, int& disconnecting, std::function<void()> on_disconnect,
               std::function<void()> on_connected = {})
      : connected_(connected),
        disconnecting_(disconnecting),
        on_disconnect_(std::move(on_disconnect)),
        on_connected_(std::move(on_connected)) {}

  void OnConnected(int32_t /*slot*/, uint64_t /*xuid*/, bool /*is_bot*/,
                   const char* /*name*/) override {
    ++connected_;
    if (on_connected_) {
      on_connected_();
    }
  }

  void OnDisconnecting(int32_t /*slot*/, uint64_t /*xuid*/) override {
    ++disconnecting_;
    if (on_disconnect_) {
      on_disconnect_();
    }
  }

 private:
  int& connected_;
  int& disconnecting_;
  std::function<void()> on_disconnect_;
  std::function<void()> on_connected_;
};

TEST(ConnectionTracker, PutInServerRecordsThenNotifiesSink) {
  // Seat a player whose engine callback still sees the slot empty.
  std::vector<std::string> events;
  auto sink = std::make_shared<RecordingSink>(events);
  ConnectionTracker::DispatchPutInServer(
      0, 76561198000000001ULL, false, "player",
      [&] {
        events.emplace_back("original");
        EXPECT_FALSE(ConnectionTracker::StateForSlot(0).occupied);
      },
      sink);

  // The slot records the player, then the sink hears of it.
  EXPECT_TRUE(ConnectionTracker::StateForSlot(0).occupied);
  EXPECT_EQ(ConnectionTracker::StateForSlot(0).xuid, 76561198000000001ULL);
  EXPECT_GT(ConnectionTracker::StateForSlot(0).generation, 0u);
  EXPECT_EQ(events,
            (std::vector<std::string>{"original", "connected:0:76561198000000001:human:player"}));
}

TEST(ConnectionTracker, DisconnectNotifiesOnceBeforeOriginalAndClearsAfter) {
  // Seat a player whose entity the engine callback removes.
  ConnectionTracker::ApplyPutInServer(3, 42, false, "p3");
  std::vector<std::string> events;
  bool entity_live = true;
  auto sink = std::make_shared<RecordingSink>(events, &entity_live);
  ConnectionTracker::DispatchDisconnect(
      3,
      [&] {
        events.emplace_back("original");
        entity_live = false;
        EXPECT_TRUE(ConnectionTracker::StateForSlot(3).occupied);
        EXPECT_EQ(ConnectionTracker::StateForSlot(3).xuid, 42);
      },
      sink);

  // The sink hears first, and the slot clears after.
  EXPECT_EQ(events, (std::vector<std::string>{"disconnecting:3:42", "original"}));
  EXPECT_FALSE(ConnectionTracker::StateForSlot(3).occupied);

  // A duplicate engine callback cannot emit another disconnecting event after
  // the tracker has cleared the slot.
  ConnectionTracker::DispatchDisconnect(
      3, [&] { events.emplace_back("duplicate original"); }, sink);
  EXPECT_EQ(events,
            (std::vector<std::string>{"disconnecting:3:42", "original", "duplicate original"}));
}

TEST(ConnectionTracker, DisconnectSinkCleansEntityBeforeEngineOriginal) {
  // Seat a player and a sink that removes their entity.
  ConnectionTracker::ApplyPutInServer(7, 77, false, "p7");
  bool entity_live = true;
  auto sink = std::make_shared<CleanupSink>(entity_live);
  int originals = 0;

  // The engine callback runs after the cleanup.
  ConnectionTracker::DispatchDisconnect(
      7,
      [&] {
        ++originals;
        EXPECT_FALSE(entity_live);
      },
      sink);

  // It runs once, and the slot clears.
  EXPECT_EQ(originals, 1);
  EXPECT_FALSE(entity_live);
  EXPECT_FALSE(ConnectionTracker::StateForSlot(7).occupied);
}

TEST(ConnectionTracker, DispatchRetainsSinkThroughEngineCallback) {
  // Seat a player and drop the caller's sink during the callback.
  ConnectionTracker::ApplyPutInServer(4, 44, false, "p4");
  std::vector<std::string> events;
  auto sink = std::make_shared<RecordingSink>(events);
  std::weak_ptr<ConnectionEventSink> weak_sink = sink;
  ConnectionTracker::DispatchDisconnect(
      4,
      [&] {
        sink.reset();
        EXPECT_FALSE(weak_sink.expired());
      },
      sink);

  // Dispatch held the sink until it returned.
  EXPECT_TRUE(weak_sink.expired());
}

TEST(ConnectionTracker, ReentrantDisconnectDispatchesOnce) {
  // Seat a player whose sink disconnects them again.
  ConnectionTracker::ApplyPutInServer(5, 55, false, "p5");
  int connected = 0;
  int disconnecting = 0;
  int originals = 0;
  std::shared_ptr<CallbackSink> sink;
  sink = std::make_shared<CallbackSink>(connected, disconnecting, [&] {
    ConnectionTracker::DispatchDisconnect(5, [&] { ++originals; }, sink);
  });

  // Disconnect them.
  ConnectionTracker::DispatchDisconnect(5, [&] { ++originals; }, sink);

  // The sink hears once and the engine once.
  EXPECT_EQ(connected, 0);
  EXPECT_EQ(disconnecting, 1);
  EXPECT_EQ(originals, 1);
  EXPECT_FALSE(ConnectionTracker::StateForSlot(5).occupied);
}

TEST(ConnectionTracker, ReconnectGenerationMayDisconnectBeforeOldCompletion) {
  // Seat a player whose sink reconnects them once, then disconnects again.
  ConnectionTracker::ApplyPutInServer(8, 88, false, "p8");
  int connected = 0;
  int disconnecting = 0;
  int originals = 0;
  bool reconnected = false;
  std::shared_ptr<CallbackSink> sink;
  sink = std::make_shared<CallbackSink>(
      connected, disconnecting,
      [&] {
        if (!reconnected) {
          reconnected = true;
          ConnectionTracker::DispatchPutInServer(
              8, 89, true, "reconnect-bot", [&] { ++originals; }, sink);
        }
      },
      [&] { ConnectionTracker::DispatchDisconnect(8, [&] { ++originals; }, sink); });

  // Disconnect them.
  ConnectionTracker::DispatchDisconnect(8, [&] { ++originals; }, sink);

  // Each generation's events arrive, and the slot ends empty.
  EXPECT_EQ(connected, 1);
  EXPECT_EQ(disconnecting, 2);
  EXPECT_EQ(originals, 3);
  EXPECT_FALSE(ConnectionTracker::StateForSlot(8).occupied);
}

TEST(ConnectionTracker, ReconnectDuringDisconnectSinkSurvivesOuterCompletion) {
  // Seat a player whose sink reconnects them.
  ConnectionTracker::ApplyPutInServer(6, 66, false, "p6");
  int connected = 0;
  int disconnecting = 0;
  int originals = 0;
  auto sink = std::make_shared<CallbackSink>(
      connected, disconnecting, [&] { ConnectionTracker::ApplyPutInServer(6, 67, false, "p6b"); });

  // Disconnect them.
  ConnectionTracker::DispatchDisconnect(6, [&] { ++originals; }, sink);

  // The reconnect survives the outer disconnect.
  EXPECT_EQ(disconnecting, 1);
  EXPECT_EQ(originals, 1);
  const auto state = ConnectionTracker::StateForSlot(6);
  EXPECT_TRUE(state.occupied);
  EXPECT_EQ(state.xuid, 67);
}

TEST(ConnectionTracker, InstallRequiresRetainedSink) {
  auto result = ConnectionTracker::Install(nullptr);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "connection tracker requires an event sink");
}

TEST(ConnectionTracker, SameXuidReconnectAdvancesGeneration) {
  // Seat, disconnect and reseat the same player.
  ConnectionTracker::ApplyPutInServer(9, 99, false, "p9");
  const auto first = ConnectionTracker::StateForSlot(9);
  ConnectionTracker::ApplyDisconnect(9);
  ConnectionTracker::ApplyPutInServer(9, 99, false, "p9");
  const auto second = ConnectionTracker::StateForSlot(9);
  EXPECT_TRUE(second.occupied);
  EXPECT_EQ(second.xuid, first.xuid);
  EXPECT_GT(second.generation, first.generation);
}

TEST(ConnectionTracker, UnoccupiedDisconnectCannotEraseReconnectFromOriginal) {
  // A callback for an empty slot may seat a player.
  ConnectionTracker::ApplyDisconnect(10);
  ConnectionTracker::DispatchDisconnect(
      10, [] { ConnectionTracker::ApplyPutInServer(10, 1010, false, "p10"); }, nullptr);
  const auto state = ConnectionTracker::StateForSlot(10);
  EXPECT_TRUE(state.occupied);
  EXPECT_EQ(state.xuid, 1010u);
}

TEST(ConnectionTracker, UnknownSlotsStayUnoccupied) {
  // Slots out of range read as empty.
  EXPECT_FALSE(ConnectionTracker::StateForSlot(-1).occupied);
  EXPECT_FALSE(ConnectionTracker::StateForSlot(64).occupied);

  // Out-of-range lifecycle events are dropped, not recorded into neighbors.
  ConnectionTracker::ApplyPutInServer(-1, 1, false, nullptr);
  ConnectionTracker::ApplyPutInServer(64, 1, false, nullptr);
  ConnectionTracker::ApplyDisconnect(-1);
  ConnectionTracker::ApplyDisconnect(64);
  EXPECT_FALSE(ConnectionTracker::StateForSlot(-1).occupied);
  EXPECT_FALSE(ConnectionTracker::StateForSlot(64).occupied);
}

TEST(ConnectionTracker, FullSignonBelongsToTheCurrentConnectionGeneration) {
  // A player signs on after the engine callback.
  ConnectionTracker::ApplyPutInServer(20, 200, false, "human");
  EXPECT_FALSE(ConnectionTracker::StateForSlot(20).fully_connected);
  ConnectionTracker::DispatchFullyConnect(
      20, [] { EXPECT_FALSE(ConnectionTracker::StateForSlot(20).fully_connected); });
  EXPECT_TRUE(ConnectionTracker::StateForSlot(20).fully_connected);

  // A reseat clears the signon.
  ConnectionTracker::ApplyPutInServer(20, 200, false, "human");
  EXPECT_FALSE(ConnectionTracker::StateForSlot(20).fully_connected);

  // A signon whose callback replaces or removes the player does not count.
  ConnectionTracker::DispatchFullyConnect(
      20, [] { ConnectionTracker::ApplyPutInServer(20, 201, false, "replacement"); });
  EXPECT_FALSE(ConnectionTracker::StateForSlot(20).fully_connected);
  ConnectionTracker::DispatchFullyConnect(20, [] { ConnectionTracker::ApplyDisconnect(20); });
  EXPECT_FALSE(ConnectionTracker::StateForSlot(20).fully_connected);

  // A bot is signed on at once.
  ConnectionTracker::ApplyPutInServer(20, 0, true, "bot");
  EXPECT_TRUE(ConnectionTracker::StateForSlot(20).fully_connected);
  ConnectionTracker::ApplyDisconnect(20);
}

TEST(ConnectionTracker, RevisionChangesWithEachJoinSignonAndLeave) {
  // A join changes the revision.
  const auto start = ConnectionTracker::Revision();
  ConnectionTracker::ApplyPutInServer(21, 210, false, "human");
  const auto joined = ConnectionTracker::Revision();
  EXPECT_NE(joined, start);

  // A signon changes it once.
  ConnectionTracker::DispatchFullyConnect(21, nullptr);
  const auto ready = ConnectionTracker::Revision();
  EXPECT_NE(ready, joined);
  EXPECT_EQ(ConnectionTracker::Revision(), ready);

  // A leave changes it.
  ConnectionTracker::ApplyDisconnect(21);
  EXPECT_NE(ConnectionTracker::Revision(), ready);

  // An event for no slot changes nothing a reader sees.
  const auto left = ConnectionTracker::Revision();
  ConnectionTracker::ApplyPutInServer(64, 1, false, nullptr);
  EXPECT_EQ(ConnectionTracker::Revision(), left);
}

}  // namespace

TEST(ConnectionTracker, TraceReceiptsCoverTheFullRosterAndReconnects) {
  // Turn tracing on, keeping the prior setting.
  const auto* prior = std::getenv("MODLOCK_INTEROP_TRACE");
  const bool had_prior = prior != nullptr;
  const std::string prior_value = prior ? prior : "";
#if defined(_WIN32)
  _putenv_s("MODLOCK_INTEROP_TRACE", "1");
#else
  setenv("MODLOCK_INTEROP_TRACE", "1", 1);
#endif

  // Capture two rounds of a full roster joining and leaving.
  testing::internal::CaptureStderr();
  for (int round = 0; round < 2; ++round) {
    for (int slot = 0; slot < 12; ++slot) {
      ConnectionTracker::DispatchPutInServer(slot, 0, true, "bot", {}, {});
      ConnectionTracker::DispatchDisconnect(slot, {}, {});
    }
  }

  // Take the trace and restore the prior setting.
  const auto trace = testing::internal::GetCapturedStderr();
#if defined(_WIN32)
  _putenv_s("MODLOCK_INTEROP_TRACE", prior_value.c_str());
#else
  if (had_prior) {
    setenv("MODLOCK_INTEROP_TRACE", prior_value.c_str(), 1);
  } else {
    unsetenv("MODLOCK_INTEROP_TRACE");
  }
#endif

  // Every slot's join appears in both rounds, and its leave.
  for (int slot = 0; slot < 12; ++slot) {
    const auto receipt = "ClientPutInServer slot " + std::to_string(slot) + " xuid 0";
    const auto first = trace.find(receipt);
    EXPECT_NE(first, std::string::npos);
    if (first != std::string::npos) {
      EXPECT_NE(trace.find(receipt, first + receipt.size()), std::string::npos);
    }
    EXPECT_NE(trace.find("ClientDisconnect slot " + std::to_string(slot) + "\n"),
              std::string::npos);
  }
}
