// Contract tests for players' mod settings: each kind accepts only its own
// values in canonical form, a missing or refused default falls back to the
// kind's first value, and a settings file keeps values across hosts.
#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "modlock/wasm_settings.h"
#include "wasm/settings.h"

namespace {

using modlock::wasm::Canonical;
using modlock::wasm::DefaultValue;
using modlock::wasm::Setting;

TEST(Settings, AcceptsEachKindsValues) {
  Setting layout;
  layout.set_kind(Setting::KIND_CHOICE);
  layout.add_choices()->set_value("corner");
  layout.add_choices()->set_value("center");
  EXPECT_EQ(Canonical(layout, "center"), "center");
  EXPECT_FALSE(Canonical(layout, "top"));

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
  EXPECT_EQ(Canonical(scale, "1.50"), "1.5");
  EXPECT_FALSE(Canonical(scale, "1.6"));
  EXPECT_FALSE(Canonical(scale, "3"));
  EXPECT_FALSE(Canonical(scale, "1.5x"));
}

TEST(Settings, DefaultsToTheDeclaredOrFirstValue) {
  Setting layout;
  layout.set_kind(Setting::KIND_CHOICE);
  layout.add_choices()->set_value("corner");
  layout.add_choices()->set_value("center");
  EXPECT_EQ(DefaultValue(layout), "corner");
  layout.set_default_value("center");
  EXPECT_EQ(DefaultValue(layout), "center");
  layout.set_default_value("top");
  EXPECT_EQ(DefaultValue(layout), "corner");

  Setting muted;
  muted.set_kind(Setting::KIND_SWITCH);
  EXPECT_EQ(DefaultValue(muted), "false");

  Setting scale;
  scale.set_kind(Setting::KIND_NUMBER);
  scale.set_min(0.5);
  scale.set_max(2);
  EXPECT_EQ(DefaultValue(scale), "0.5");
}

TEST(FileSettings, KeepsValuesAcrossHosts) {
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
