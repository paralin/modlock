#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "modlock/export.h"

namespace modlock::gameinterop {

// FrameTimingSummary describes the server frames of one reporting window.
// Interval is the time from one frame's start to the next frame's start; work
// is the time a frame spent in GameFrame and modlock's frame callbacks, which
// engine and modlock split: the engine's GameFrame, then modlock's mods. Late
// frames started more than twice the median interval after the previous one.
struct FrameTimingSummary {
  uint32_t frames = 0;
  double seconds = 0;
  double interval_p50_ms = 0;
  double interval_p99_ms = 0;
  double interval_max_ms = 0;
  double work_p50_ms = 0;
  double work_p99_ms = 0;
  double work_max_ms = 0;
  double engine_p50_ms = 0;
  double engine_p99_ms = 0;
  double engine_max_ms = 0;
  double modlock_p50_ms = 0;
  double modlock_p99_ms = 0;
  double modlock_max_ms = 0;
  uint32_t late = 0;
};

// FrameTiming accumulates server frame timing into fixed histograms of
// kBucket resolution up to kRange, so recording a frame never allocates.
// Longer frames count in the last bucket and still set the exact maximum.
class MODLOCK_API FrameTiming {
 public:
  using Clock = std::chrono::steady_clock;

  static constexpr Clock::duration kWindow = std::chrono::seconds(60);
  static constexpr Clock::duration kBucket = std::chrono::microseconds(100);
  static constexpr Clock::duration kRange = std::chrono::milliseconds(250);

  // Record adds a frame that ran from start to end, in which the engine's own
  // frame ended at engine_end and modlock's work took the rest. Once the window
  // that began at its first frame spans kWindow, Record returns its summary and
  // starts the next window.
  std::optional<FrameTimingSummary> Record(Clock::time_point start, Clock::time_point engine_end,
                                           Clock::time_point end);

 private:
  static constexpr size_t kBuckets = kRange / kBucket;

  // Histogram counts durations by bucket and keeps their exact maximum.
  struct Histogram {
    std::array<uint32_t, kBuckets> counts{};
    uint32_t total = 0;
    Clock::duration max{};

    void Add(Clock::duration duration);
    [[nodiscard]] Clock::duration Percentile(double fraction) const;
    [[nodiscard]] uint32_t CountAbove(Clock::duration duration) const;
  };

  [[nodiscard]] FrameTimingSummary Summarize(Clock::time_point now) const;

  std::optional<Clock::time_point> previous_start_;
  Clock::time_point window_start_;
  uint32_t frames_ = 0;
  Histogram intervals_;
  Histogram work_;
  Histogram engine_;
  Histogram modlock_;
};

// ModFrames totals one mod's frames for the summary the log takes of them
// once a minute: on a production server, the only timing a mod gets.
class MODLOCK_API ModFrames {
 public:
  using Clock = FrameTiming::Clock;

  static constexpr Clock::duration kPeriod = std::chrono::minutes(1);
  // kOver is the time a mod may spend of a server frame.
  static constexpr Clock::duration kOver = std::chrono::milliseconds(4);

  // Record adds a frame of the mod that ran from start to end. Once the
  // period that began at its first frame spans kPeriod, Record returns the
  // period's summary line, such as "frames: 3600 in 60 s, mean 1.20 ms, max
  // 9.10 ms, 3 over 4 ms", and starts the next period.
  std::optional<std::string> Record(Clock::time_point start, Clock::time_point end);

 private:
  Clock::time_point started_;
  uint32_t frames_ = 0;
  uint32_t over_ = 0;
  Clock::duration spent_{};
  Clock::duration slowest_{};
};

// FormatFrameTiming renders summary as one log line.
MODLOCK_API std::string FormatFrameTiming(const FrameTimingSummary& summary);

}  // namespace modlock::gameinterop
