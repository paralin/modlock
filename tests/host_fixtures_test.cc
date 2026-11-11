// Contract tests for a mod's needed services and the host's fixtures: a mod
// whose needed service nobody provides does not start and its failure names
// the service, an optional one starts the mod, a fixture handed to the host
// at start answers each method with its reply, for its own mod only, and a
// forwarded service reaches the controller over the control link and ends
// with its answer.
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "host_app/control_link.h"
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

// Forwarded encodes a fixture of shout for arena and a forward of it for
// extension-go.
std::string Forwarded() {
  modlock::host_app::control::Fixtures fixtures;
  fixtures.ParseFromString(Fixtures("arena"));
  auto* forward = fixtures.add_forwards();
  forward->set_mod("extension-go");
  forward->set_service("shout");
  forward->set_target("https://service.test");
  return fixtures.SerializeAsString();
}

TEST(HostFixtures, AForwardedServiceTakesItsModsStartedCalls) {
  // Forwarding without a forward is refused.
  WasmHost host({});
  ASSERT_FALSE(modlock::host_app::ProvideFixtures(host, Forwarded()));

  // The forward takes the mod's started call, and the log names its target.
  Build build(R"([{"name":"shout"}])");
  std::vector<std::string> calls;
  std::vector<modlock::WasmReply> replies;
  auto provided = modlock::host_app::ProvideFixtures(
      host, Forwarded(),
      [&](std::string_view mod, std::string_view service, std::string_view method,
          std::string_view payload, modlock::WasmReply reply) {
        calls.push_back(std::string(mod) + " " + std::string(service) + " " +
                        std::string(method) + " " + std::string(payload));
        replies.push_back(std::move(reply));
      });
  ASSERT_TRUE(provided) << provided.error();
  EXPECT_EQ(*provided,
            "arena: service shout: fixture services/shout.json\n"
            "extension-go: service shout: forwarded to https://service.test\n");
  auto plugin = host.Load(build.Path(), PluginContext{.check_only = true});
  ASSERT_TRUE(plugin) << plugin.error();
  ASSERT_TRUE((*plugin)->Start());
  ASSERT_TRUE(host.Call("extension-go", "later", "start", "shout|say|hi|0"));
  ASSERT_EQ(calls, std::vector<std::string>{"extension-go shout say hi"});

  // Its answer ends the call in the next frame.
  replies[0](std::string("HI"));
  (*plugin)->Tick();
  auto ended = host.Call("extension-go", "later", "ended", "");
  ASSERT_TRUE(ended) << ended.error();
  EXPECT_EQ(*ended, "hi HI");
  (*plugin)->Stop();
}

#ifndef _WIN32
// Controller is a loopback controller that one control link dials.
class Controller {
 public:
  Controller() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    ::bind(listener_, reinterpret_cast<sockaddr*>(&address), size);
    ::listen(listener_, 1);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
    port_ = ntohs(address.sin_port);
  }
  ~Controller() {
    Hangup();
    ::close(listener_);
  }
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  std::string Address() const { return "127.0.0.1:" + std::to_string(port_); }
  void Accept() { conn_ = ::accept(listener_, nullptr, nullptr); }
  void Hangup() {
    if (conn_ >= 0) {
      ::close(conn_);
      conn_ = -1;
    }
  }

  // Next reads the link's next event.
  modlock::host_app::control::HostEvent Next() {
    std::array<unsigned char, 4> header{};
    Read(reinterpret_cast<char*>(header.data()), header.size());
    const uint32_t size = header[0] | header[1] << 8 | header[2] << 16 | header[3] << 24;
    std::string data(size, '\0');
    Read(data.data(), data.size());
    modlock::host_app::control::HostEvent event;
    event.ParseFromString(data);
    return event;
  }

  // Send writes one request to the link.
  void Send(const modlock::host_app::control::ControlRequest& request) {
    const auto data = request.SerializeAsString();
    const auto size = static_cast<uint32_t>(data.size());
    const std::array<char, 4> header{static_cast<char>(size), static_cast<char>(size >> 8),
                                     static_cast<char>(size >> 16), static_cast<char>(size >> 24)};
    ::send(conn_, header.data(), header.size(), 0);
    ::send(conn_, data.data(), data.size(), 0);
  }

 private:
  void Read(char* data, size_t size) {
    while (size > 0) {
      const auto got = ::recv(conn_, data, size, 0);
      ASSERT_GT(got, 0);
      data += got;
      size -= static_cast<size_t>(got);
    }
  }

  int listener_ = -1;
  int conn_ = -1;
  uint16_t port_ = 0;
};

// Until runs the link's requests until done reports true, for two seconds at
// most.
bool Until(modlock::host_app::ControlLink& link, WasmHost& host, const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    link.RunRequests(host);
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

TEST(HostFixtures, TheControlLinkAsksTheControllerAndEndsTheCallWithItsAnswer) {
  // Dial a controller.
  Controller controller;
  auto dialed = modlock::host_app::ControlLink::Dial(controller.Address());
  ASSERT_TRUE(dialed) << dialed.error();
  controller.Accept();
  auto& link = **dialed;
  WasmHost host({});

  // A forwarded call reaches the controller with its id and fields.
  std::vector<std::expected<std::string, std::string>> ended;
  const auto keep = [&ended](std::expected<std::string, std::string> answer) {
    ended.push_back(std::move(answer));
  };
  link.Forward("drills", "example.drills.v1", "read", R"({"key":"slide"})", keep);
  auto event = controller.Next();
  ASSERT_TRUE(event.has_service_call());
  const auto call = event.service_call();
  EXPECT_EQ(call.mod(), "drills");
  EXPECT_EQ(call.service(), "example.drills.v1");
  EXPECT_EQ(call.method(), "read");
  EXPECT_EQ(call.payload(), R"({"key":"slide"})");

  // An answer for another id does nothing; the call's own answer ends it.
  modlock::host_app::control::ControlRequest stray;
  stray.mutable_answer()->set_id(call.id() + 7);
  controller.Send(stray);
  modlock::host_app::control::ControlRequest answer;
  answer.mutable_answer()->set_id(call.id());
  answer.mutable_answer()->set_payload(R"({"name":"Slide"})");
  controller.Send(answer);
  ASSERT_TRUE(Until(link, host, [&] { return !ended.empty(); }));
  ASSERT_EQ(ended.size(), 1u);
  ASSERT_TRUE(ended[0]) << ended[0].error();
  EXPECT_EQ(*ended[0], R"({"name":"Slide"})");

  // An error answer fails the call with its text.
  link.Forward("drills", "example.drills.v1", "save", "{}", keep);
  const auto second = controller.Next().service_call();
  modlock::host_app::control::ControlRequest refused;
  refused.mutable_answer()->set_id(second.id());
  refused.mutable_answer()->set_error("Only the mod's developers may call it");
  controller.Send(refused);
  ASSERT_TRUE(Until(link, host, [&] { return ended.size() == 2; }));
  ASSERT_FALSE(ended[1]);
  EXPECT_EQ(ended[1].error(), "Only the mod's developers may call it");

  // A call left unanswered when the controller goes ends with that, and so
  // does a call made after.
  link.Forward("drills", "example.drills.v1", "read", "{}", keep);
  controller.Next();
  controller.Hangup();
  ASSERT_TRUE(Until(link, host, [&] { return ended.size() == 3; }));
  EXPECT_EQ(ended[2].error(), "the program that answers this service has gone");
  link.Forward("drills", "example.drills.v1", "read", "{}", keep);
  ASSERT_EQ(ended.size(), 4u);
  EXPECT_EQ(ended[3].error(), "the program that answers this service has gone");
}
#endif

}  // namespace
