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
using std::chrono::microseconds;
using std::chrono::milliseconds;

// RunFrames records frames every interval, each working for work, until one
// returns a summary, and returns it.
std::optional<FrameTimingSummary> RunFrames(FrameTiming& timing, FrameTiming::Clock::time_point& at,
                                            microseconds interval, microseconds work, int frames) {
  for (int frame = 0; frame < frames; ++frame) {
    if (auto summary = timing.Record(at, at + work)) return summary;
    at += interval;
  }
  return std::nullopt;
}

TEST(FrameTiming, SummarizesOneSteadyWindow) {
  FrameTiming timing;
  FrameTiming::Clock::time_point at{};
  const auto summary = RunFrames(timing, at, microseconds(16'667), microseconds(3'050), 4000);

  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->frames, 3601u);
  EXPECT_NEAR(summary->seconds, 60.0, 0.01);
  EXPECT_NEAR(summary->interval_p50_ms, 16.667, 1e-9);
  EXPECT_NEAR(summary->interval_max_ms, 16.667, 1e-9);
  EXPECT_NEAR(summary->work_p50_ms, 3.05, 1e-9);
  EXPECT_EQ(summary->late, 0u);
}

TEST(FrameTiming, CountsLateFramesAndKeepsTheExactMaximum) {
  FrameTiming timing;
  FrameTiming::Clock::time_point at{};
  ASSERT_FALSE(RunFrames(timing, at, microseconds(16'667), microseconds(2'000), 100));
  at += milliseconds(300);
  ASSERT_FALSE(RunFrames(timing, at, microseconds(16'667), microseconds(2'000), 1));
  const auto summary = RunFrames(timing, at, microseconds(16'667), microseconds(2'000), 4000);

  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->late, 1u);
  EXPECT_NEAR(summary->interval_max_ms, 316.667, 1e-9);
}

TEST(FrameTiming, StartsTheNextWindowAfterASummary) {
  FrameTiming timing;
  FrameTiming::Clock::time_point at{};
  ASSERT_TRUE(RunFrames(timing, at, microseconds(16'667), microseconds(1'000), 4000));
  at += microseconds(16'667);
  const auto next = RunFrames(timing, at, microseconds(16'667), microseconds(1'000), 4000);

  ASSERT_TRUE(next);
  EXPECT_EQ(next->frames, 3601u);
  EXPECT_NEAR(next->interval_p50_ms, 16.667, 1e-9);
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
      .late = 2,
  };
  EXPECT_EQ(FormatFrameTiming(summary),
            "[modlock] frames: 3600 in 60.0s, interval p50 16.7 p99 17.3 max 48.2 ms, "
            "work p50 3.1 p99 6.4 max 31.0 ms, late 2");
}

}  // namespace
