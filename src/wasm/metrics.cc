#include "wasm/metrics.h"

#include <algorithm>
#include <format>

namespace modlock::wasm {

const Metric* FindMetric(const Manifest& manifest, std::string_view name) {
  for (const auto& metric : manifest.metrics()) {
    if (metric.name() == name) return &metric;
  }
  return nullptr;
}

bool Labeled(const Metric& metric, std::string_view label) {
  if (metric.labels().empty()) return label.empty();
  return std::ranges::find(metric.labels(), label) != metric.labels().end();
}

void Tally::Add(const Metric& metric, std::string_view label, double value) {
  auto [total, added] = totals_.try_emplace({metric.name(), std::string(label)}, 0.0);
  switch (metric.kind()) {
    case Metric::KIND_COUNT:
      total->second += 1;
      break;
    case Metric::KIND_SUM:
      total->second += value;
      break;
    case Metric::KIND_MAX:
      total->second = added ? value : std::max(total->second, value);
      break;
    default:
      break;
  }
}

MetricTotals Tally::Totals() const {
  MetricTotals totals;
  for (const auto& [key, value] : totals_) {
    auto& total = *totals.add_totals();
    total.set_name(key.first);
    total.set_label(key.second);
    total.set_value(value);
  }
  return totals;
}

std::string Tally::Text() const {
  std::string text;
  for (const auto& [key, value] : totals_) {
    if (!text.empty()) text += ", ";
    text += key.first;
    if (!key.second.empty()) text += "[" + key.second + "]";
    text += std::format(" {}", value);
  }
  return text;
}

}  // namespace modlock::wasm
