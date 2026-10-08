// Contract tests for extension services: the host calls a service a mod
// serves, the mod calls a service the host provides, and each error reaches
// the caller. A mod that holds its running build keeps it through a reload
// until it releases the hold. A mod that stops restarts with backoff.
#include "modlock/wasm_host.h"

#include <cctype>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace {

using modlock::PluginContext;
using modlock::WasmHost;
using modlock::WasmHostObserver;

// Events records what happens to the host's mods.
class Events final : public WasmHostObserver {
 public:
  void Started(std::string_view, bool reloaded) override {
    seen.push_back(reloaded ? "reloaded" : "started");
  }
  void Logged(std::string_view, std::string_view) override {}
  void Failed(std::string_view, std::string_view error) override {
    seen.push_back("failed: " + std::string(error));
  }
  void Ui(std::string_view, int32_t, const modlock::ui::Change&) override {}
  void Held(std::string_view, bool held) override { seen.push_back(held ? "held" : "released"); }

  std::vector<std::string> seen;
};

// Shout answers with the method and the payload in capitals, or refuses the
// method "refuse".
std::expected<std::string, std::string> Shout(std::string_view mod, std::string_view method,
                                              std::string_view payload) {
  if (method == "refuse") return std::unexpected(std::string(mod) + " may not refuse");
  std::string answer = std::string(method) + ":" + std::string(payload);
  for (auto& c : answer) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return answer;
}

TEST(WasmHost, ModsAndHostCallEachOthersServices) {
  WasmHost host({});
  host.Provide("shout", Shout);
  auto plugin = host.Load(EXTENSION_GO_WASM, PluginContext{.check_only = true});
  ASSERT_TRUE(plugin) << plugin.error();
  ASSERT_TRUE((*plugin)->Start());

  // A call travels through the mod to the host's service and back.
  auto answer = host.Call("extension-go", "echo", "say", "hi");
  ASSERT_TRUE(answer) << answer.error();
  EXPECT_EQ(*answer, "SAY:HI");

  // The host's error reaches the mod, which returns it to the host.
  auto refused = host.Call("extension-go", "echo", "refuse", "");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), "extension-go may not refuse");

  // A service the mod does not serve, and a mod that is not loaded, fail.
  auto missing = host.Call("extension-go", "missing", "say", "");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), "the mod serves no service missing");
  EXPECT_FALSE(host.Call("absent", "echo", "say", ""));

  // A withdrawn service fails the mod's call.
  host.Provide("shout", nullptr);
  auto withdrawn = host.Call("extension-go", "echo", "say", "hi");
  ASSERT_FALSE(withdrawn);
  EXPECT_EQ(withdrawn.error(), "this host provides no service shout");
  (*plugin)->Stop();
}

TEST(WasmHost, HeldModKeepsItsBuildUntilReleased) {
  WasmHost host({});
  Events events;
  host.Observe(&events);
  auto plugin = host.Load(EXTENSION_GO_WASM, PluginContext{.check_only = true});
  ASSERT_TRUE(plugin) << plugin.error();
  ASSERT_TRUE((*plugin)->Start());

  // A reload of a held mod succeeds but leaves the running build in place.
  ASSERT_TRUE(host.Call("extension-go", "hold", "take", ""));
  EXPECT_TRUE(host.Held("extension-go"));
  ASSERT_TRUE(host.Reload(EXTENSION_GO_WASM));
  EXPECT_EQ(events.seen, (std::vector<std::string>{"started", "held"}));

  // The waiting build replaces the running one on the frame after release,
  // and starts without a hold.
  ASSERT_TRUE(host.Call("extension-go", "hold", "release", ""));
  EXPECT_EQ(events.seen, (std::vector<std::string>{"started", "held", "released"}));
  (*plugin)->Tick();
  EXPECT_EQ(events.seen, (std::vector<std::string>{"started", "held", "released", "reloaded"}));
  EXPECT_FALSE(host.Held("extension-go"));

  // Without a hold, a reload applies at once.
  ASSERT_TRUE(host.Reload(EXTENSION_GO_WASM));
  EXPECT_EQ(events.seen.back(), "reloaded");
  (*plugin)->Stop();
  host.Unobserve(&events);
}

TEST(WasmHost, StoppedModRestarts) {
  WasmHost host({});
  host.Provide("shout", Shout);
  Events events;
  host.Observe(&events);
  auto plugin = host.Load(EXTENSION_GO_WASM, PluginContext{.check_only = true});
  ASSERT_TRUE(plugin) << plugin.error();
  ASSERT_TRUE((*plugin)->Start());

  // The first stop restarts the mod on the next frame, serving again.
  EXPECT_FALSE(host.Call("extension-go", "crash", "now", ""));
  EXPECT_FALSE(host.Call("extension-go", "echo", "say", "hi"));
  (*plugin)->Tick();
  ASSERT_EQ(events.seen.size(), 3u);
  EXPECT_TRUE(events.seen[1].starts_with("failed: stopped: "));
  EXPECT_EQ(events.seen[2], "reloaded");
  auto answer = host.Call("extension-go", "echo", "say", "hi");
  ASSERT_TRUE(answer) << answer.error();
  EXPECT_EQ(*answer, "SAY:HI");

  // A second stop waits a second, so the next frame leaves it stopped.
  EXPECT_FALSE(host.Call("extension-go", "crash", "now", ""));
  (*plugin)->Tick();
  EXPECT_EQ(events.seen.size(), 4u);
  EXPECT_FALSE(host.Call("extension-go", "echo", "say", "hi"));
  (*plugin)->Stop();
  host.Unobserve(&events);
}

}  // namespace
