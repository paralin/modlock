// Contract tests for players' mod settings: each kind accepts only its own
// values in canonical form, a missing or refused default falls back to the
// kind's first value, and a settings file keeps values across hosts.
#include "modlock/wasm_settings.h"

#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "wasm/settings.h"

namespace {

using modlock::wasm::Canonical;
using modlock::wasm::DefaultValue;
using modlock::wasm::Setting;

TEST(Settings, AcceptsEachKindsValues) {
  // A choice accepts only its listed values.
  Setting layout;
  layout.set_kind(Setting::KIND_CHOICE);
  layout.add_choices()->set_value("corner");
  layout.add_choices()->set_value("center");
  EXPECT_EQ(Canonical(layout, "center"), "center");
  EXPECT_FALSE(Canonical(layout, "top"));

  // A switch accepts only true and false.
  Setting muted;
  muted.set_kind(Setting::KIND_SWITCH);
  EXPECT_EQ(Canonical(muted, "true"), "true");
  EXPECT_FALSE(Canonical(muted, "1"));

  // A number must lie within the bounds and on a step from min.
  Setting scale;
  scale.set_kind(Setting::KIND_NUMBER);
  scale.set_min(0.5);
  scale.set_max(2);
  scale.set_step(0.25);

  // A number prints in canonical form; one off a step, out of range or malformed is refused.
  EXPECT_EQ(Canonical(scale, "1.50"), "1.5");
  EXPECT_FALSE(Canonical(scale, "1.6"));
  EXPECT_FALSE(Canonical(scale, "3"));
  EXPECT_FALSE(Canonical(scale, "1.5x"));
}

TEST(Settings, DefaultsToTheDeclaredOrFirstValue) {
  // Set up a choice of two.
  Setting layout;
  layout.set_kind(Setting::KIND_CHOICE);
  layout.add_choices()->set_value("corner");
  layout.add_choices()->set_value("center");

  // A choice defaults to its declared value when listed, else to its first.
  EXPECT_EQ(DefaultValue(layout), "corner");
  layout.set_default_value("center");
  EXPECT_EQ(DefaultValue(layout), "center");
  layout.set_default_value("top");
  EXPECT_EQ(DefaultValue(layout), "corner");

  // A switch defaults to off.
  Setting muted;
  muted.set_kind(Setting::KIND_SWITCH);
  EXPECT_EQ(DefaultValue(muted), "false");

  // A number defaults to its minimum.
  Setting scale;
  scale.set_kind(Setting::KIND_NUMBER);
  scale.set_min(0.5);
  scale.set_max(2);
  EXPECT_EQ(DefaultValue(scale), "0.5");
}

TEST(FileSettings, KeepsValuesAcrossHosts) {
  // Store values in one host and read them in the next.
  const auto path = std::filesystem::path(testing::TempDir()) / "modlock-settings-test.json";
  std::filesystem::remove(path);
  {
    modlock::FileSettings settings(path);
    EXPECT_FALSE(settings.Value("hud", 7, "layout"));
    settings.Store("hud", 7, "layout", "corner");
    settings.Store("hud", 7, "layout", "center");
    settings.Store("hud", 8, "layout", "corner");
  }
  modlock::FileSettings settings(path);
  EXPECT_EQ(settings.Value("hud", 7, "layout"), "center");
  EXPECT_EQ(settings.Value("hud", 8, "layout"), "corner");
  EXPECT_FALSE(settings.Value("other", 7, "layout"));
  std::filesystem::remove(path);
}

}  // namespace
