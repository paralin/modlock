#include "modlock/gameinterop/frame_timing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
    if (seen >= rank && seen > 0) return std::min<Clock::duration>(max, kBucket * (bucket + 1));
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
                                                      Clock::time_point end) {
  if (frames_ == 0) window_start_ = start;
  if (previous_start_) intervals_.Add(start - *previous_start_);
  previous_start_ = start;
  work_.Add(end - start);
  ++frames_;
  if (end - window_start_ < kWindow) return std::nullopt;

  const FrameTimingSummary summary = Summarize(end);
  frames_ = 0;
  intervals_ = {};
  work_ = {};
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
      .late = intervals_.CountAbove(2 * median),
  };
}

std::string FormatFrameTiming(const FrameTimingSummary& summary) {
  char line[256];
  std::snprintf(line, sizeof(line),
                "[modlock] frames: %u in %.1fs, interval p50 %.1f p99 %.1f max %.1f ms, "
                "work p50 %.1f p99 %.1f max %.1f ms, late %u",
                summary.frames, summary.seconds, summary.interval_p50_ms, summary.interval_p99_ms,
                summary.interval_max_ms, summary.work_p50_ms, summary.work_p99_ms,
                summary.work_max_ms, summary.late);
  return line;
}

}  // namespace modlock::gameinterop
