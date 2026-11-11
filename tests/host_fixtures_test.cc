// Contract tests for a mod's needed services and the host's fixtures: a mod
// whose needed service nobody provides does not start and its failure names
// the service, an optional one starts the mod, and a fixture handed to the
// host at start answers each method with its reply, for its own mod only.
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "host_app/fixtures.h"
#include "modlock/wasm_host.h"
#include "src/host_app/control/control.pb.h"

namespace {

using modlock::PluginContext;
using modlock::WasmHost;
using modlock::WasmHostObserver;

// Failures records the failures the host's mods report.
class Failures final : public WasmHostObserver {
 public:
  void Started(std::string_view, bool) override {}
  void Logged(std::string_view, std::string_view) override {}
  void Failed(std::string_view, std::string_view error) override { seen.emplace_back(error); }
  void Ui(std::string_view, int32_t, const modlock::wasm::ui::Change&) override {}

  std::vector<std::string> seen;
};

// Build stages the test mod under a mod.json that names services.
class Build {
 public:
  explicit Build(std::string_view services)
      : root_(std::filesystem::temp_directory_path() / "modlock-host-fixtures-test") {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    std::filesystem::copy_file(EXTENSION_GO_WASM, root_ / "mod.wasm");
    std::ofstream(root_ / "mod.json")
        << R"({"slug":"extension-go","runtime":"RUNTIME_WASM","entry":"mod.wasm","services":)" << services << "}";
  }
  ~Build() { std::filesystem::remove_all(root_); }
  Build(const Build&) = delete;
  Build& operator=(const Build&) = delete;

  const std::filesystem::path& Path() const { return root_; }

 private:
  std::filesystem::path root_;
};

// Fixtures encodes one reply of service shout for mod.
std::string Fixtures(std::string_view mod) {
  modlock::host_app::control::Fixtures fixtures;
  auto* fixture = fixtures.add_fixtures();
  fixture->set_mod(std::string(mod));
  fixture->set_service("shout");
  fixture->set_method("say");
  fixture->set_payload(R"({"said":"hi"})");
  fixture->set_source("services/shout.json");
  return fixtures.SerializeAsString();
}

TEST(HostFixtures, ANeededServiceNobodyProvidesStopsTheStart) {
  // With nothing provided, the mod that needs shout does not start, and its
  // failure names the service.
  Build build(R"([{"name":"shout"},{"name":"ghost","optional":true}])");
  WasmHost host({});
  Failures failures;
  host.Observe(&failures);
  auto plugin = host.Load(build.Path(), PluginContext{.check_only = true});
  ASSERT_TRUE(plugin) << plugin.error();
  EXPECT_FALSE((*plugin)->Start());
  ASSERT_EQ(failures.seen.size(), 1u);
  EXPECT_EQ(failures.seen[0], "the mod needs service shout, which nobody provides here");

  // A service answered later counts as provided; the optional ghost does not
  // hold the start.
  host.ProvideLater("shout", [](std::string_view, std::string_view, std::string_view,
                                modlock::WasmReply reply) { reply(std::string("late")); });
  EXPECT_TRUE((*plugin)->Start());
  (*plugin)->Stop();
  host.Unobserve(&failures);
}

TEST(HostFixtures, AFixtureAnswersItsModsCalls) {
  // The fixture provides shout, so the mod that needs it starts, and the host
  // logs who answers.
  Build build(R"([{"name":"shout"}])");
  WasmHost host({});
  auto provided = modlock::host_app::ProvideFixtures(host, Fixtures("extension-go"));
  ASSERT_TRUE(provided) << provided.error();
  EXPECT_EQ(*provided, "extension-go: service shout: fixture services/shout.json\n");
  auto plugin = host.Load(build.Path(), PluginContext{.check_only = true});
  ASSERT_TRUE(plugin) << plugin.error();
  ASSERT_TRUE((*plugin)->Start());

  // A method's call gets its reply; a method the fixture lacks is refused,
  // naming the file.
  auto answer = host.Call("extension-go", "echo", "say", "");
  ASSERT_TRUE(answer) << answer.error();
  EXPECT_EQ(*answer, R"({"said":"hi"})");
  auto missing = host.Call("extension-go", "echo", "sing", "");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), "the fixture services/shout.json has no method sing");
  (*plugin)->Stop();
}

TEST(HostFixtures, AFixtureAnswersNoOtherMod) {
  // Another mod's fixture of the same service does not answer this one.
  Build build(R"([{"name":"shout","optional":true}])");
  WasmHost host({});
  ASSERT_TRUE(modlock::host_app::ProvideFixtures(host, Fixtures("arena")));
  auto plugin = host.Load(build.Path(), PluginContext{.check_only = true});
  ASSERT_TRUE(plugin) << plugin.error();
  ASSERT_TRUE((*plugin)->Start());
  auto refused = host.Call("extension-go", "echo", "say", "");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), "this host has no fixture of this service for extension-go");
  (*plugin)->Stop();

  // A file that is not a Fixtures message provides nothing.
  EXPECT_FALSE(modlock::host_app::ProvideFixtures(host, "\xff\xff\xff"));
}

}  // namespace
