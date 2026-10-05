#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "proto/modlock/wasm.pb.h"

namespace modlock::wasm {

// FindMetric returns the metric named name that manifest declares, or null.
const Metric* FindMetric(const Manifest& manifest, std::string_view name);

// Labeled reports whether a call may label metric with label: one of its
// labels, or empty for a metric without labels.
bool Labeled(const Metric& metric, std::string_view label);

// Tally keeps one player's totals of one mod's metrics.
class Tally {
 public:
  // Add adds value to metric's total under label.
  void Add(const Metric& metric, std::string_view label, double value);

  // Totals returns the totals in name and label order.
  MetricTotals Totals() const;

  // Text describes the totals in one line, such as "hud_opened[compact] 3".
  std::string Text() const;

  bool Empty() const { return totals_.empty(); }

 private:
  // totals_ holds each total by metric name and label.
  std::map<std::pair<std::string, std::string>, double, std::less<>> totals_;
};

}  // namespace modlock::wasm
