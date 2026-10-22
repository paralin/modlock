// Contract tests for mod metrics: declared labels, and how each kind
// combines the values added to it.
#include "gtest/gtest.h"
#include "wasm/metrics.h"

namespace modlock::wasm {
namespace {

Manifest Declared() {
  // Declare a labeled count.
  Manifest manifest;
  auto& opened = *manifest.add_metrics();
  opened.set_name("hud.opened");
  opened.set_kind(Metric::KIND_COUNT);
  opened.add_labels("classic");
  opened.add_labels("compact");

  // Declare a sum and a maximum.
  auto& damage = *manifest.add_metrics();
  damage.set_name("damage");
  damage.set_kind(Metric::KIND_SUM);
  auto& combo = *manifest.add_metrics();
  combo.set_name("best_combo");
  combo.set_kind(Metric::KIND_MAX);
  return manifest;
}

TEST(Metrics, AcceptOnlyDeclaredLabels) {
  // A labeled metric accepts only its labels.
  const auto manifest = Declared();
  const auto* opened = FindMetric(manifest, "hud.opened");
  ASSERT_NE(opened, nullptr);
  EXPECT_TRUE(Labeled(*opened, "compact"));
  EXPECT_FALSE(Labeled(*opened, "minimal"));
  EXPECT_FALSE(Labeled(*opened, ""));

  // An unlabeled metric accepts only the empty label, and an undeclared one is absent.
  EXPECT_TRUE(Labeled(*FindMetric(manifest, "damage"), ""));
  EXPECT_FALSE(Labeled(*FindMetric(manifest, "damage"), "compact"));
  EXPECT_EQ(FindMetric(manifest, "missing"), nullptr);
}

TEST(Metrics, TallyCombinesEachKind) {
  // Start an empty tally.
  const auto manifest = Declared();
  Tally tally;
  EXPECT_TRUE(tally.Empty());

  // Add a count under two labels, a sum and a maximum.
  tally.Add(*FindMetric(manifest, "hud.opened"), "compact", 5);
  tally.Add(*FindMetric(manifest, "hud.opened"), "compact", 5);
  tally.Add(*FindMetric(manifest, "hud.opened"), "classic", 0);
  tally.Add(*FindMetric(manifest, "damage"), "", 12.5);
  tally.Add(*FindMetric(manifest, "damage"), "", 7.5);
  tally.Add(*FindMetric(manifest, "best_combo"), "", -3);
  tally.Add(*FindMetric(manifest, "best_combo"), "", -7);

  // The text and totals combine each kind, ordered by name and label.
  EXPECT_EQ(tally.Text(), "best_combo -3, damage 20, hud.opened[classic] 1, hud.opened[compact] 2");
  const auto totals = tally.Totals();
  ASSERT_EQ(totals.totals_size(), 4);
  EXPECT_EQ(totals.totals(3).name(), "hud.opened");
  EXPECT_EQ(totals.totals(3).label(), "compact");
  EXPECT_EQ(totals.totals(3).value(), 2);
}

}  // namespace
}  // namespace modlock::wasm
