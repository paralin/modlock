// Contract tests for extension services: the host calls a service a mod
// serves, the mod calls a service the host provides, and each error reaches
// the caller. A mod that holds its running build keeps it through a reload
// until it releases the hold. A mod that stops stays stopped until a player
// or a reload restarts it. A mod limited to some players reaches only them,
// changes the whole server only while it holds the world, which one limited
// mod holds at a time, and rules only the hits of its players. A mod hears
// exactly one leave for each player it heard join.
#include "modlock/wasm_host.h"

#include <cctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "wasm/attendance.h"
#include "wasm/call_profile.h"
#include "wasm/player_scope.h"

namespace {

using modlock::PluginContext;
using modlock::WasmHost;
using modlock::WasmHostObserver;
using modlock::WasmScope;
using Contact = modlock::wasm::PlayerScope::Contact;

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

  void Ui(std::string_view, int32_t, const modlock::wasm::ui::Change&) override {}

  void Held(std::string_view, bool held) override { seen.push_back(held ? "held" : "released"); }

  void Called(std::string_view mod, const modlock::wasm::profile::CallProfile& reported) override {
    if (mod == "extension-go") {
      calls.MergeFrom(reported);
    }
  }

  // Key returns the counts calls holds for method and key, or empty counts.
  modlock::wasm::profile::Counts Key(std::string_view method, std::string_view key) const {
    for (const auto& kept : calls.keys()) {
      if (kept.method() == method && kept.key() == key) {
        return kept.counts();
      }
    }
    return {};
  }

  std::vector<std::string> seen;
  modlock::wasm::profile::CallProfile calls;
};

// Shout answers with the method and the payload in capitals, or refuses the
// method "refuse".
std::expected<std::string, std::string> Shout(std::string_view mod, std::string_view method,
                                              std::string_view payload) {
  if (method == "refuse") {
    return std::unexpected(std::string(mod) + " may not refuse");
  }
  std::string answer = std::string(method) + ":" + std::string(payload);
  for (auto& c : answer) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return answer;
}

TEST(WasmHost, ModsAndHostCallEachOthersServices) {
  // Load the test mod with the shout service.
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

  // A mod limited to some players is refused a request that addresses
  // another player or the whole server, ahead of the check's own refusal.
  ASSERT_TRUE(host.Scope("extension-go", {.players = std::set<int32_t>{1}}));
  auto served = host.Call("extension-go", "reach", "show", "1");
  ASSERT_FALSE(served);
  EXPECT_EQ(served.error(), "no game is running");
  auto other = host.Call("extension-go", "reach", "show", "2");
  ASSERT_FALSE(other);
  EXPECT_EQ(other.error(), "this mod does not serve the player ShowEntityOnlyTo names");
  auto server = host.Call("extension-go", "reach", "command", "");
  ASSERT_FALSE(server);
  EXPECT_EQ(server.error(), "this mod serves only some players and may not call ServerCommand");

  // Holding the world lets it change the whole server, still only for its
  // players' requests, and a second limited mod may not hold it too.
  ASSERT_TRUE(host.Scope("extension-go", {.players = std::set<int32_t>{1}, .world = true}));
  auto world = host.Call("extension-go", "reach", "command", "");
  ASSERT_FALSE(world);
  EXPECT_EQ(world.error(), "no game is running");
  auto still = host.Call("extension-go", "reach", "show", "2");
  ASSERT_FALSE(still);
  EXPECT_EQ(still.error(), "this mod does not serve the player ShowEntityOnlyTo names");
  // A second instance of the same build runs under its own name, for its own
  // players, while the first keeps its name and scope.
  EXPECT_FALSE(host.Load(EXTENSION_GO_WASM, PluginContext{.check_only = true}));
  auto second = host.Load(EXTENSION_GO_WASM, PluginContext{.check_only = true}, "second");
  ASSERT_TRUE(second) << second.error();
  ASSERT_TRUE((*second)->Start());
  EXPECT_STREQ((*second)->Name(), "second");
  ASSERT_TRUE(host.Scope("second", {.players = std::set<int32_t>{2}}));
  auto own = host.Call("second", "reach", "show", "2");
  ASSERT_FALSE(own);
  EXPECT_EQ(own.error(), "no game is running");
  auto first = host.Call("second", "reach", "show", "1");
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error(), "this mod does not serve the player ShowEntityOnlyTo names");
  auto taken = host.Scope("second", {.players = std::set<int32_t>{2}, .world = true});
  ASSERT_FALSE(taken);
  EXPECT_EQ(taken.error(), "the mod extension-go already changes the world");
  ASSERT_TRUE(host.Scope("extension-go", {.players = std::set<int32_t>{1}}));
  ASSERT_TRUE(host.Scope("second", {.players = std::set<int32_t>{2}, .world = true}));
  (*second)->Stop();

  // Serving every player again lifts every limit.
  ASSERT_TRUE(host.Scope("extension-go", {}));
  auto unlimited = host.Call("extension-go", "reach", "show", "2");
  ASSERT_FALSE(unlimited);
  EXPECT_EQ(unlimited.error(), "no game is running");
  EXPECT_FALSE(host.Scope("absent", {}));

  // A withdrawn service fails the mod's call.
  host.Provide("shout", nullptr);
  auto withdrawn = host.Call("extension-go", "echo", "say", "hi");
  ASSERT_FALSE(withdrawn);
  EXPECT_EQ(withdrawn.error(), "this host provides no service shout");

  // The mod's calls reach the observers when its build stops, counted by
  // method and by what they name. A command a mod may not run, a file name
  // outside its files, a method the host lacks and a call outside its scope
  // are refused under their own key; a call past the kept keys overflows.
  ASSERT_TRUE(host.Call("extension-go", "probe", "", ""));
  Events events;
  host.Observe(&events);
  (*plugin)->Stop();
  host.Unobserve(&events);
  EXPECT_EQ(events.Key("CallService", "shout").calls(), 3);
  EXPECT_EQ(events.Key("ServerCommand", "echo").calls(), 2);
  EXPECT_EQ(events.Key("ServerCommand", "echo").refused(), 1);
  EXPECT_EQ(events.Key("ServerCommand", "say").calls(), 1);
  EXPECT_EQ(events.Key("ServerCommand", "say").refused(), 0);
  EXPECT_EQ(events.Key("ServerCommand", "exec").refused(), 1);
  EXPECT_EQ(events.Key("ReadFile", "../outside").refused(), 1);
  EXPECT_EQ(events.Key("", "NoSuchMethod").refused(), 1);
  EXPECT_EQ(events.calls.keys_size(), modlock::wasm::CallProfile::kMaxKeys);
  EXPECT_GT(events.calls.overflow(), 0);
  for (const auto& method : events.calls.methods()) {
    if (method.method() == "ReadFile") {
      EXPECT_EQ(method.counts().calls(), 301);
      EXPECT_GE(method.counts().refused(), 1);
    }
  }
}

TEST(PlayerScope, LimitedScopeRulesOnlyItsPlayersHits) {
  // A limited scope hears hits among its players, blocks hits that cross to
  // another player, and leaves others' hits alone. A hit with one player,
  // such as on a trooper or from a hazard, follows that player.
  modlock::wasm::PlayerScope scope;
  scope.Limit({.players = std::set<int32_t>{1, 2}, .apart = true});
  EXPECT_EQ(scope.Hit(1, 2), Contact::kHear);
  EXPECT_EQ(scope.Hit(1, 1), Contact::kHear);
  EXPECT_EQ(scope.Hit(1, 3), Contact::kBlock);
  EXPECT_EQ(scope.Hit(3, 2), Contact::kBlock);
  EXPECT_EQ(scope.Hit(3, 4), Contact::kPass);
  EXPECT_EQ(scope.Hit(1, std::nullopt), Contact::kHear);
  EXPECT_EQ(scope.Hit(std::nullopt, 2), Contact::kHear);
  EXPECT_EQ(scope.Hit(3, std::nullopt), Contact::kPass);
  EXPECT_EQ(scope.Hit(std::nullopt, std::nullopt), Contact::kPass);
  EXPECT_EQ(scope.Apart(), 0b110u);
  EXPECT_FALSE(scope.World());

  // An unlimited scope hears every hit and holds the world, and apart means
  // nothing without players to keep apart.
  scope.Limit({.apart = true});
  EXPECT_EQ(scope.Hit(3, 4), Contact::kHear);
  EXPECT_EQ(scope.Hit(std::nullopt, std::nullopt), Contact::kHear);
  EXPECT_EQ(scope.Apart(), 0u);
  EXPECT_TRUE(scope.World());
}

TEST(Attendance, PairsEachJoinWithOneLeave) {
  namespace players = modlock::wasm::players;
  // Connection is a roster entry for slot.
  auto connection = [](int32_t slot, uint32_t generation, bool ready) {
    players::Connection entry;
    entry.set_player(slot);
    entry.set_steam_id(76561198000000000ULL + slot);
    entry.set_generation(generation);
    entry.set_ready(ready);
    return entry;
  };
  google::protobuf::RepeatedPtrField<players::Connection> roster;
  *roster.Add() = connection(1, 1, true);
  *roster.Add() = connection(2, 1, false);

  // Only a ready player joins, once; one still joining joins when ready.
  modlock::wasm::Attendance attendance;
  auto changes = attendance.Reconcile(roster);
  ASSERT_EQ(changes.joined.size(), 1u);
  EXPECT_EQ(changes.joined[0].connection().steam_id(), 76561198000000001ULL);
  EXPECT_TRUE(changes.left.empty());
  EXPECT_TRUE(attendance.Reconcile(roster).joined.empty());
  roster[1].set_ready(true);
  changes = attendance.Reconcile(roster);
  ASSERT_EQ(changes.joined.size(), 1u);
  EXPECT_EQ(changes.joined[0].connection().player(), 2);

  // A kick leaves with its reason, while the player is still listed, and
  // the report that drops them adds no second leave.
  auto kicked = attendance.Leave(1, players::DISCONNECT_REASON_KICKED);
  ASSERT_TRUE(kicked);
  EXPECT_TRUE(kicked->disconnected());
  EXPECT_EQ(kicked->reason(), players::DISCONNECT_REASON_KICKED);
  EXPECT_EQ(kicked->connection().steam_id(), 76561198000000001ULL);
  EXPECT_FALSE(attendance.Leave(1, players::DISCONNECT_REASON_KICKED));
  roster.DeleteSubrange(0, 1);
  EXPECT_TRUE(attendance.Reconcile(roster).left.empty());

  // A player the mod stops serving leaves without disconnecting.
  auto unserved = attendance.Leave(2, std::nullopt);
  ASSERT_TRUE(unserved);
  EXPECT_FALSE(unserved->disconnected());
  EXPECT_FALSE(unserved->has_reason());

  // Served again, they join again; a disconnect the engine gives no reason
  // for leaves without one.
  EXPECT_EQ(attendance.Reconcile(roster).joined.size(), 1u);
  auto unexplained = attendance.Leave(2, 0);
  ASSERT_TRUE(unexplained);
  EXPECT_TRUE(unexplained->disconnected());
  EXPECT_FALSE(unexplained->has_reason());

  // A report that shows a missed disconnect, a missing slot or a new
  // connection in the slot, leaves the old one without a reason.
  attendance.Clear();
  EXPECT_EQ(attendance.Reconcile(roster).joined.size(), 1u);
  roster[0] = connection(2, 2, true);
  changes = attendance.Reconcile(roster);
  ASSERT_EQ(changes.left.size(), 1u);
  EXPECT_EQ(changes.left[0].connection().generation(), 1u);
  EXPECT_TRUE(changes.left[0].disconnected());
  EXPECT_FALSE(changes.left[0].has_reason());
  ASSERT_EQ(changes.joined.size(), 1u);
  EXPECT_EQ(changes.joined[0].connection().generation(), 2u);
  roster.Clear();
  EXPECT_EQ(attendance.Reconcile(roster).left.size(), 1u);
}

TEST(WasmHost, HeldModKeepsItsBuildUntilReleased) {
  // Load the test mod, watching its events.
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

TEST(WasmHost, StoppedModWaitsForRestart) {
  // Load the test mod with the shout service, watching its events.
  WasmHost host({});
  host.Provide("shout", Shout);
  Events events;
  host.Observe(&events);
  auto plugin = host.Load(EXTENSION_GO_WASM, PluginContext{.check_only = true});
  ASSERT_TRUE(plugin) << plugin.error();
  ASSERT_TRUE((*plugin)->Start());

  // A stopped mod stays stopped through later frames.
  EXPECT_FALSE(host.Call("extension-go", "crash", "now", ""));
  (*plugin)->Tick();
  ASSERT_EQ(events.seen.size(), 2u);
  EXPECT_TRUE(events.seen[1].starts_with("failed: stopped: "));
  EXPECT_FALSE(host.Call("extension-go", "echo", "say", "hi"));

  // A reload starts it again, serving.
  ASSERT_TRUE(host.Reload(EXTENSION_GO_WASM));
  EXPECT_EQ(events.seen.back(), "reloaded");
  auto answer = host.Call("extension-go", "echo", "say", "hi");
  ASSERT_TRUE(answer) << answer.error();
  EXPECT_EQ(*answer, "SAY:HI");
  (*plugin)->Stop();
  host.Unobserve(&events);
}

TEST(WasmHost, LoadsOnlyABuildNamedByASlug) {
  // A build whose manifest names anything but a slug does not load.
  WasmHost host({});
  const auto build = std::filesystem::temp_directory_path() / "modlock-slug";
  std::filesystem::create_directories(build);
  for (const char* slug : {"..", "a b", "a;quit", "Arena", "a--b", "a-"}) {
    std::ofstream(build / "mod.json") << R"({"slug": ")" << slug << R"(", "entry": "mod.wasm"})";
    auto plugin = host.Load(build, PluginContext{.check_only = true});
    ASSERT_FALSE(plugin) << slug;
    EXPECT_TRUE(plugin.error().ends_with("invalid slug")) << plugin.error();
  }

  // A slug passes, and the load goes on to the entry this build lacks.
  std::ofstream(build / "mod.json") << R"({"slug": "arena-2", "entry": "mod.wasm"})";
  auto named = host.Load(build, PluginContext{.check_only = true});
  ASSERT_FALSE(named);
  EXPECT_TRUE(named.error().starts_with("cannot read ")) << named.error();
  std::filesystem::remove_all(build);
}

}  // namespace
