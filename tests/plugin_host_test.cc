// Contract tests for the plugin host: version negotiation, lifecycle order,
// and tick accounting across multiple plugins.
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "modlock/host.h"

namespace {

using modlock::Plugin;
using modlock::host::PluginHost;

constexpr uint32_t kVersion = modlock::PluginInterfaceVersion;

// CountingPlugin records the lifecycle calls it receives, in order.
class CountingPlugin final : public Plugin {
 public:
  explicit CountingPlugin(std::string name) : name_(std::move(name)) {}

  uint32_t InterfaceVersion() const override { return version_; }
  const char* Name() const override { return name_.c_str(); }
  bool Load() override {
    events_.emplace_back("load");
    return load_ok_;
  }
  bool Start() override {
    events_.emplace_back("start");
    return start_ok_;
  }
  void Tick() override { events_.emplace_back("tick"); }
  void Stop() override { events_.emplace_back("stop"); }

  void set_version(uint32_t v) { version_ = v; }
  void set_load_ok(bool v) { load_ok_ = v; }
  void set_start_ok(bool v) { start_ok_ = v; }
  const std::vector<std::string>& events() const { return events_; }

 private:
  std::string name_;
  uint32_t version_ = kVersion;
  bool load_ok_ = true;
  bool start_ok_ = true;
  std::vector<std::string> events_;
};

TEST(PluginHost, RejectsVersionMismatchWithoutRegistering) {
  // Build a plugin one version ahead.
  PluginHost host;
  auto bad = std::make_unique<CountingPlugin>("too-new");
  bad->set_version(kVersion + 1);

  // Register it.
  auto error = host.Register(std::move(bad));

  // It is refused and not listed.
  ASSERT_NE(error, nullptr);
  EXPECT_NE(error->find("does not match host version"), std::string::npos);
  EXPECT_EQ(host.Plugins().size(), 0u);
}

TEST(PluginHost, RejectsDuplicateNames) {
  // A second plugin of the same name is refused.
  PluginHost host;
  auto error = host.Register(std::make_unique<CountingPlugin>("dup"));
  EXPECT_EQ(error, nullptr);
  error = host.Register(std::make_unique<CountingPlugin>("dup"));
  ASSERT_NE(error, nullptr);
  EXPECT_NE(error->find("duplicate"), std::string::npos);
}

TEST(PluginHost, DrivesLifecycleAndCountsTicksPerPlugin) {
  // Build two plugins, keeping a view of each.
  PluginHost host;
  auto a = std::make_unique<CountingPlugin>("a");
  auto b = std::make_unique<CountingPlugin>("b");
  auto* raw_a = a.get();
  auto* raw_b = b.get();

  // Register both.
  ASSERT_EQ(host.Register(std::move(a)), nullptr);
  ASSERT_EQ(host.Register(std::move(b)), nullptr);

  // Start them.
  std::string error;
  ASSERT_TRUE(host.StartAll(error)) << error;

  // Tick five times, then stop.
  for (int i = 0; i < 5; i++) {
    host.TickAll();
  }
  host.StopAll();

  // StopAll is idempotent and Tick after Stop does nothing.
  host.StopAll();
  host.TickAll();

  // Each plugin counts its own ticks.
  for (const auto& p : host.Plugins()) {
    // Ticks freeze at the stop boundary: the TickAll after StopAll must not
    // advance them past 5.
    EXPECT_EQ(p.ticks, 5u) << "ticked after StopAll: " << p.name;
  }

  // load, start, five ticks, stop.
  ASSERT_EQ(raw_a->events().size(), 8u);
  EXPECT_EQ(raw_b->events().size(), 8u);
  EXPECT_EQ(raw_a->events()[0], "load");
  EXPECT_EQ(raw_a->events()[1], "start");
  EXPECT_EQ(raw_a->events().back(), "stop");
}

TEST(PluginHost, LoadFailureRollsBackTheWholeStartedSet) {
  // Build a healthy plugin and one whose Load fails.
  PluginHost host;
  auto ok_plugin = std::make_unique<CountingPlugin>("healthy");
  auto bad = std::make_unique<CountingPlugin>("broken");
  bad->set_load_ok(false);
  auto* healthy = ok_plugin.get();
  auto* broken = bad.get();

  // Register both.
  ASSERT_EQ(host.Register(std::move(ok_plugin)), nullptr);
  ASSERT_EQ(host.Register(std::move(bad)), nullptr);

  // Starting fails and names the broken plugin.
  std::string error;
  EXPECT_FALSE(host.StartAll(error));
  EXPECT_NE(error.find("broken: Load returned false"), std::string::npos);

  // Nothing ticks; the healthy plugin is stopped, and a later start fails.
  host.TickAll();
  host.StopAll();
  const auto plugins = host.Plugins();
  ASSERT_EQ(plugins.size(), 2u);
  EXPECT_EQ(plugins[0].ticks, 0u);
  EXPECT_EQ(healthy->events(), (std::vector<std::string>{"load", "start", "stop"}));
  EXPECT_EQ(broken->events(), (std::vector<std::string>{"load", "stop"}));
  EXPECT_FALSE(host.StartAll(error));
}

TEST(PluginHost, StartFailureReleasesLoadedResourcesExactlyOnce) {
  // Register a plugin whose Start fails.
  PluginHost host;
  auto plugin = std::make_unique<CountingPlugin>("partial");
  plugin->set_start_ok(false);
  auto* observed = plugin.get();
  ASSERT_EQ(host.Register(std::move(plugin)), nullptr);

  // The failed start stops it once, and the host takes no new plugins.
  std::string error;
  EXPECT_FALSE(host.StartAll(error));
  EXPECT_NE(error.find("partial: Start returned false"), std::string::npos);
  host.StopAll();
  host.TickAll();
  EXPECT_EQ(observed->events(), (std::vector<std::string>{"load", "start", "stop"}));
  EXPECT_NE(host.Register(std::make_unique<CountingPlugin>("late")), nullptr);
}

}  // namespace
