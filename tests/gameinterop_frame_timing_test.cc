// Contract tests for server frame timing: window boundaries, interval and work
// percentiles, late-frame counting and the log line.
#include <gtest/gtest.h>

#include <chrono>
#include <optional>

#include "modlock/gameinterop/frame_timing.h"

namespace {

using modlock::gameinterop::FormatFrameTiming;
using modlock::gameinterop::FrameTiming;
using modlock::gameinterop::FrameTimingSummary;
using modlock::gameinterop::ModFrames;
using std::chrono::microseconds;
using std::chrono::milliseconds;

// RunFrames records frames every interval, each working for work, of which
// the mods take mods, until one returns a summary, and returns it.
std::optional<FrameTimingSummary> RunFrames(FrameTiming& timing, FrameTiming::Clock::time_point& at,
                                            microseconds interval, microseconds work, int frames,
                                            microseconds mods = microseconds(0)) {
  for (int frame = 0; frame < frames; ++frame) {
    if (auto summary = timing.Record(at, at + work - mods, at + work)) {
      return summary;
    }
    at += interval;
  }
  return std::nullopt;
}

TEST(FrameTiming, SummarizesOneSteadyWindow) {
  // Run a minute of steady frames.
  FrameTiming timing;
  FrameTiming::Clock::time_point at{};
  const auto summary = RunFrames(timing, at, microseconds(16'667), microseconds(3'050), 4000);

  // One window summarizes them.
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->frames, 3601u);
  EXPECT_NEAR(summary->seconds, 60.0, 0.01);
  EXPECT_NEAR(summary->interval_p50_ms, 16.667, 1e-9);
  EXPECT_NEAR(summary->interval_max_ms, 16.667, 1e-9);
  EXPECT_NEAR(summary->work_p50_ms, 3.05, 1e-9);
  EXPECT_EQ(summary->late, 0u);
}

TEST(FrameTiming, CountsLateFramesAndKeepsTheExactMaximum) {
  // Run frames with one 300 ms stall.
  FrameTiming timing;
  FrameTiming::Clock::time_point at{};
  ASSERT_FALSE(RunFrames(timing, at, microseconds(16'667), microseconds(2'000), 100));
  at += milliseconds(300);
  ASSERT_FALSE(RunFrames(timing, at, microseconds(16'667), microseconds(2'000), 1));
  const auto summary = RunFrames(timing, at, microseconds(16'667), microseconds(2'000), 4000);

  // The stall is late, and its interval is the maximum.
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->late, 1u);
  EXPECT_NEAR(summary->interval_max_ms, 316.667, 1e-9);
}

TEST(FrameTiming, StartsTheNextWindowAfterASummary) {
  // Finish one window and run another.
  FrameTiming timing;
  FrameTiming::Clock::time_point at{};
  ASSERT_TRUE(RunFrames(timing, at, microseconds(16'667), microseconds(1'000), 4000));
  at += microseconds(16'667);
  const auto next = RunFrames(timing, at, microseconds(16'667), microseconds(1'000), 4000);

  // The next window counts its own frames.
  ASSERT_TRUE(next);
  EXPECT_EQ(next->frames, 3601u);
  EXPECT_NEAR(next->interval_p50_ms, 16.667, 1e-9);
}

TEST(FrameTiming, SplitsTheEngineFromTheMods) {
  // Run a minute of frames in which the mods take 1.2 ms of 3.05 ms.
  FrameTiming timing;
  FrameTiming::Clock::time_point at{};
  const auto summary =
      RunFrames(timing, at, microseconds(16'667), microseconds(3'050), 4000, microseconds(1'250));

  // The engine's share and the mods' share come apart.
  ASSERT_TRUE(summary);
  EXPECT_NEAR(summary->work_p50_ms, 3.05, 1e-9);
  EXPECT_NEAR(summary->engine_p50_ms, 1.8, 1e-9);
  EXPECT_NEAR(summary->engine_max_ms, 1.8, 1e-9);
  EXPECT_NEAR(summary->modlock_p50_ms, 1.25, 1e-9);
  EXPECT_NEAR(summary->modlock_max_ms, 1.25, 1e-9);
}

TEST(FrameTiming, FormatsOneLogLine) {
  const FrameTimingSummary summary{
      .frames = 3600,
      .seconds = 60.02,
      .interval_p50_ms = 16.7,
      .interval_p99_ms = 17.3,
      .interval_max_ms = 48.25,
      .work_p50_ms = 3.1,
      .work_p99_ms = 6.4,
      .work_max_ms = 31.0,
      .engine_p50_ms = 1.9,
      .engine_p99_ms = 2.6,
      .engine_max_ms = 7.0,
      .modlock_p50_ms = 1.2,
      .modlock_p99_ms = 3.8,
      .modlock_max_ms = 24.0,
      .late = 2,
  };
  EXPECT_EQ(FormatFrameTiming(summary),
            "[modlock] frames: 3600 in 60.0s, interval p50 16.7 p99 17.3 max 48.2 ms, "
            "work p50 3.1 p99 6.4 max 31.0 ms (engine p50 1.9 p99 2.6 max 7.0, "
            "mods p50 1.2 p99 3.8 max 24.0), late 2");
}

TEST(ModFrames, SummarizesAMinuteOfOneModsFrames) {
  // Run a minute of a mod's frames, every hundredth of them slow.
  ModFrames mod;
  ModFrames::Clock::time_point at{};
  std::optional<std::string> line;
  int frames = 0;
  for (; !line && frames < 5000; ++frames) {
    const auto spent = frames % 100 == 99 ? microseconds(9'100) : microseconds(1'100);
    line = mod.Record(at, at + spent);
    at += microseconds(16'667);
  }

  // One line sums the minute, and the next minute counts afresh.
  ASSERT_TRUE(line);
  EXPECT_EQ(frames, 3601);
  EXPECT_EQ(*line, "frames: 3601 in 60 s, mean 1.18 ms, max 9.10 ms, 36 over 4 ms");
  EXPECT_FALSE(mod.Record(at, at + microseconds(1'000)));
}

}  // namespace
