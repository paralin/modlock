// Contract tests for extension services: the host calls a service a mod
// serves, the mod calls a service the host provides, and each error reaches
// the caller.
#include <cctype>
#include <expected>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "modlock/wasm_host.h"

namespace {

using modlock::PluginContext;
using modlock::WasmHost;

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

}  // namespace
