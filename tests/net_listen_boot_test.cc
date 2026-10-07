#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "modlock/net/listen_boot.h"

namespace {

// The staging prefix retains the reference loader's server flags and disables
// user-config persistence for the shared game installation. Commands use the
// authenticated loopback ingress instead of an engine RCON listener.
constexpr std::string_view kReferenceStaging =
    "-dedicated -nodedicatedconsole -novconsole -dev -insecure"
    " -allow_no_lobby_connect -playtest"
    " +tv_citadel_auto_record 0 +spec_replay_enable 0 +tv_enable 0"
    " +citadel_upload_replay_enabled 0";

std::string_view StagingPrefix(const std::string& line) {
  const auto hostport = line.find(" +hostport ");
  return hostport == std::string::npos ? line : std::string_view{line}.substr(0, hostport);
}

TEST(NetListenBoot, CommandLineCarriesReferenceStagingPrefix) {
  EXPECT_EQ(StagingPrefix(modlock::net::BuildDedicatedCommandLine({})), kReferenceStaging);
}

TEST(NetListenBoot, CommandLineStartsDedicatedServer) {
  const std::string line = modlock::net::BuildDedicatedCommandLine({});
  EXPECT_NE(line.find("-dedicated"), std::string::npos);
  EXPECT_NE(line.find("-insecure"), std::string::npos);
  EXPECT_NE(line.find("+hostport 27067"), std::string::npos);
  EXPECT_NE(line.find("+ip 0.0.0.0"), std::string::npos);
  EXPECT_NE(line.find("+map dl_midtown"), std::string::npos);
}

TEST(NetListenBoot, CommandLineCarriesOverrides) {
  const modlock::net::LaunchConfig config{.host_port = 27016, .map = "dl_bomb"};
  const std::string line = modlock::net::BuildDedicatedCommandLine(config);
  EXPECT_NE(line.find("+hostport 27016"), std::string::npos);
  EXPECT_NE(line.find("+map dl_bomb"), std::string::npos);
}

TEST(NetListenBoot, ClientCommandLineJoinsWithoutServerFlags) {
  const std::string line = modlock::net::BuildClientCommandLine({.connect = "127.0.0.1:27079"});
  EXPECT_NE(line.find("+connect 127.0.0.1:27079"), std::string::npos);
  EXPECT_NE(line.find("-insecure"), std::string::npos);
  EXPECT_NE(line.find("+deadlock_early_development_warning_disabled 1"), std::string::npos);
  EXPECT_EQ(line.find("-dedicated"), std::string::npos);
  EXPECT_EQ(line.find("-playtest"), std::string::npos);
  EXPECT_EQ(line.find("+map"), std::string::npos);
}

TEST(NetListenBoot, HandoffRefusesWithoutMappedEngine) {
  // No game module is mapped in a test process; the handoff must refuse
  // instead of starting. The error names the missing precondition.
  const auto result = modlock::net::RunEngine({}, ".");
  ASSERT_FALSE(result.has_value());
  EXPECT_FALSE(result.error().empty());
}

}  // namespace
