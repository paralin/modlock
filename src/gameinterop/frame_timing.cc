#include "modlock/gameinterop/frame_timing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

namespace modlock::gameinterop {
namespace {

double Milliseconds(FrameTiming::Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

}  // namespace

void FrameTiming::Histogram::Add(Clock::duration duration) {
  const auto bucket = static_cast<size_t>(std::max<Clock::rep>(0, duration / kBucket));
  ++counts[std::min(bucket, kBuckets - 1)];
  ++total;
  max = std::max(max, duration);
}

// Percentile returns the upper edge of the bucket that holds the given
// fraction of durations, capped at the exact maximum.
FrameTiming::Clock::duration FrameTiming::Histogram::Percentile(double fraction) const {
  const auto rank = static_cast<uint32_t>(std::ceil(fraction * total));
  uint32_t seen = 0;
  for (size_t bucket = 0; bucket < kBuckets; ++bucket) {
    seen += counts[bucket];
    if (seen >= rank && seen > 0) {
      return std::min<Clock::duration>(max, kBucket * (bucket + 1));
    }
  }
  return max;
}

// CountAbove counts the durations in buckets that start above duration.
uint32_t FrameTiming::Histogram::CountAbove(Clock::duration duration) const {
  uint32_t count = 0;
  for (size_t bucket = duration / kBucket + 1; bucket < kBuckets; ++bucket) {
    count += counts[bucket];
  }
  return count;
}

std::optional<FrameTimingSummary> FrameTiming::Record(Clock::time_point start,
                                                      Clock::time_point engine_end,
                                                      Clock::time_point end) {
  // Add the frame to the window.
  if (frames_ == 0) {
    window_start_ = start;
  }
  if (previous_start_) {
    intervals_.Add(start - *previous_start_);
  }
  previous_start_ = start;
  work_.Add(end - start);
  engine_.Add(engine_end - start);
  modlock_.Add(end - engine_end);
  ++frames_;
  if (end - window_start_ < kWindow) {
    return std::nullopt;
  }

  // Summarize a full window and start the next.
  const FrameTimingSummary summary = Summarize(end);
  frames_ = 0;
  intervals_ = {};
  work_ = {};
  engine_ = {};
  modlock_ = {};
  return summary;
}

FrameTimingSummary FrameTiming::Summarize(Clock::time_point now) const {
  const Clock::duration median = intervals_.Percentile(0.5);
  return {
      .frames = frames_,
      .seconds = std::chrono::duration<double>(now - window_start_).count(),
      .interval_p50_ms = Milliseconds(median),
      .interval_p99_ms = Milliseconds(intervals_.Percentile(0.99)),
      .interval_max_ms = Milliseconds(intervals_.max),
      .work_p50_ms = Milliseconds(work_.Percentile(0.5)),
      .work_p99_ms = Milliseconds(work_.Percentile(0.99)),
      .work_max_ms = Milliseconds(work_.max),
      .engine_p50_ms = Milliseconds(engine_.Percentile(0.5)),
      .engine_p99_ms = Milliseconds(engine_.Percentile(0.99)),
      .engine_max_ms = Milliseconds(engine_.max),
      .modlock_p50_ms = Milliseconds(modlock_.Percentile(0.5)),
      .modlock_p99_ms = Milliseconds(modlock_.Percentile(0.99)),
      .modlock_max_ms = Milliseconds(modlock_.max),
      .late = intervals_.CountAbove(2 * median),
  };
}

std::optional<std::string> ModFrames::Record(Clock::time_point start, Clock::time_point end) {
  // Add the frame to the period.
  if (frames_ == 0) {
    started_ = start;
  }
  const auto spent = end - start;
  ++frames_;
  spent_ += spent;
  slowest_ = std::max(slowest_, spent);
  if (spent > kOver) {
    ++over_;
  }
  if (end - started_ < kPeriod) {
    return std::nullopt;
  }

  // Summarize a full period and start the next.
  const auto line =
      std::format("frames: {} in {:.0f} s, mean {:.2f} ms, max {:.2f} ms, {} over 4 ms", frames_,
                  std::chrono::duration<double>(end - started_).count(),
                  Milliseconds(spent_) / frames_, Milliseconds(slowest_), over_);
  *this = {};
  return line;
}

std::string FormatFrameTiming(const FrameTimingSummary& summary) {
  char line[384];
  std::snprintf(line, sizeof(line),
                "[modlock] frames: %u in %.1fs, interval p50 %.1f p99 %.1f max %.1f ms, "
                "work p50 %.1f p99 %.1f max %.1f ms (engine p50 %.1f p99 %.1f max %.1f, "
                "mods p50 %.1f p99 %.1f max %.1f), late %u",
                summary.frames, summary.seconds, summary.interval_p50_ms, summary.interval_p99_ms,
                summary.interval_max_ms, summary.work_p50_ms, summary.work_p99_ms,
                summary.work_max_ms, summary.engine_p50_ms, summary.engine_p99_ms,
                summary.engine_max_ms, summary.modlock_p50_ms, summary.modlock_p99_ms,
                summary.modlock_max_ms, summary.late);
  return line;
}

}  // namespace modlock::gameinterop
