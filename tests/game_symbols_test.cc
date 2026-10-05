#include "modlock/gameinterop/game_symbols.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "modlock/gameinterop/signature.h"

namespace {

using modlock::gameinterop::FindGameSignature;
using modlock::gameinterop::GameModule;
using modlock::gameinterop::GameSignature;
using modlock::gameinterop::GameSignatures;
using modlock::gameinterop::LoadGameSignatures;
using modlock::gameinterop::ModuleImage;
using modlock::gameinterop::ResolveSignature;
using modlock::gameinterop::SignatureTarget;

class Image final : public ModuleImage {
 public:
  std::uintptr_t base() const override { return 0x1000; }
  std::span<const uint8_t> image_bytes() const override { return bytes; }

  std::array<uint8_t, 32> bytes{};
};

GameSignature Match(std::string_view pattern) { return {.id = "target", .pattern = pattern}; }

GameSignature Relative(std::string_view pattern, SignatureTarget target, size_t delta) {
  return {.id = "relative", .pattern = pattern, .target = target, .delta = delta};
}

// GameDataFile writes text to a game data file named for the running test.
std::filesystem::path GameDataFile(std::string_view text) {
  const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
  const auto path =
      std::filesystem::temp_directory_path() / (std::string("modlock-") + test->name() + ".txtpb");
  std::ofstream(path) << text;
  return path;
}

TEST(GameSignatures, TableIsSortedUniqueAndParses) {
  const auto signatures = GameSignatures();
  ASSERT_FALSE(signatures.empty());
  for (size_t i = 0; i < signatures.size(); ++i) {
    const auto& signature = signatures[i];
    if (i > 0) EXPECT_LT(signatures[i - 1].id, signature.id) << "sort and dedupe the table";
    EXPECT_NE(signature.modules, 0) << signature.id;
    EXPECT_FALSE(signature.shape.empty()) << signature.id;
    const auto parsed =
        modlock::gameinterop::ParseSignature(std::string(signature.id), signature.pattern);
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_FALSE(parsed->bytes.empty()) << signature.id;
    if (signature.target == SignatureTarget::kMatch) EXPECT_EQ(signature.delta, 0) << signature.id;
    EXPECT_EQ(FindGameSignature(signature.id), &signature);
  }
  EXPECT_EQ(FindGameSignature("not.recorded"), nullptr);
}

TEST(GameSignatures, ModulesNameTheirFiles) {
  EXPECT_EQ(modlock::gameinterop::GameModuleFile(GameModule::kServer), "server.dll");
  EXPECT_EQ(modlock::gameinterop::GameModuleFile(GameModule::kClient), "client.dll");
  EXPECT_EQ(modlock::gameinterop::GameModuleFile(GameModule::kEngine), "engine2.dll");
  const GameSignature shared{.modules = GameModule::kServer | GameModule::kClient};
  EXPECT_TRUE(shared.In(GameModule::kClient));
  EXPECT_FALSE(shared.In(GameModule::kEngine));
}

TEST(GameSymbols, EveryTargetRequiresOneMatch) {
  Image image;
  image.bytes[4] = 0xcc;
  auto symbol = ResolveSignature(image, Match("CC"));
  ASSERT_TRUE(symbol) << symbol.error();
  EXPECT_EQ(*symbol, reinterpret_cast<void*>(0x1004));

  image.bytes[16] = 0xcc;
  for (const auto pattern : {"CC", "FF", "", "ZZ"}) {
    EXPECT_FALSE(ResolveSignature(image, Match(pattern)));
    EXPECT_FALSE(ResolveSignature(image, Relative(pattern, SignatureTarget::kCall, 0)));
    EXPECT_FALSE(ResolveSignature(image, Relative(pattern, SignatureTarget::kRipRelative, 0)));
  }
  const auto unknown = ResolveSignature(image, "not.recorded");
  ASSERT_FALSE(unknown);
  EXPECT_NE(unknown.error().find("not.recorded"), std::string::npos);
}

TEST(GameSymbols, RelativeInstructionsPreserveSignedDisplacements) {
  for (const int32_t displacement : {-16, 16}) {
    Image image;
    image.bytes[4] = 0xcc;
    image.bytes[5] = 0xe8;
    std::memcpy(image.bytes.data() + 6, &displacement, sizeof(displacement));
    auto call = ResolveSignature(image, Relative("CC E8", SignatureTarget::kCall, 1));
    ASSERT_TRUE(call) << call.error();
    EXPECT_EQ(*call, reinterpret_cast<void*>(0x100a + displacement));

    image.bytes[5] = 0x48;
    image.bytes[6] = 0x8d;
    image.bytes[7] = 0x05;
    std::memcpy(image.bytes.data() + 8, &displacement, sizeof(displacement));
    auto lea = ResolveSignature(image, Relative("CC 48 8D 05", SignatureTarget::kRipRelative, 1));
    ASSERT_TRUE(lea) << lea.error();
    EXPECT_EQ(*lea, reinterpret_cast<void*>(0x100c + displacement));
  }
}

TEST(GameSymbols, RelativeInstructionsRejectTruncationAndWrappingOffsets) {
  Image image;
  image.bytes[4] = 0xcc;
  for (const size_t delta : {size_t{28}, std::numeric_limits<size_t>::max()}) {
    EXPECT_FALSE(ResolveSignature(image, Relative("CC", SignatureTarget::kCall, delta)));
    EXPECT_FALSE(ResolveSignature(image, Relative("CC", SignatureTarget::kRipRelative, delta)));
  }
  image.bytes[30] = 0xe8;
  EXPECT_FALSE(ResolveSignature(image, Relative("E8", SignatureTarget::kCall, 0)));
  EXPECT_FALSE(ResolveSignature(image, Relative("E8", SignatureTarget::kRipRelative, 0)));
  EXPECT_FALSE(ResolveSignature(image, Relative("CC", SignatureTarget::kCall, 0)));
}

// The load tests replace the recorded signatures, so they end by loading the
// shipped game data file.

TEST(GameSignatureLoad, BrokenEntriesAreSkippedAlone) {
  const auto load = LoadGameSignatures(GameDataFile(R"(
signatures {
  id: "b.call"
  modules: GAME_MODULE_CLIENT
  modules: GAME_MODULE_ENGINE
  pattern: "E8 ?? ?? ?? ??"
  call_at: 0
  shape: "a call"
}
signatures { id: "a.match" modules: GAME_MODULE_SERVER pattern: "CC" shape: "a match" }
signatures { id: "a.match" modules: GAME_MODULE_SERVER pattern: "DD" shape: "a repeat" }
signatures { id: "c.no-module" pattern: "CC" }
signatures { id: "d.bad-pattern" modules: GAME_MODULE_SERVER pattern: "ZZ" }
signatures { modules: GAME_MODULE_SERVER pattern: "CC" }
)"));
  ASSERT_TRUE(load) << load.error();
  EXPECT_EQ(load->loaded, 2);
  ASSERT_EQ(load->skipped.size(), 4);
  for (const auto id : {"a.match", "c.no-module", "d.bad-pattern"}) {
    EXPECT_TRUE(std::ranges::any_of(load->skipped, [id](const auto& reason) {
      return reason.contains(id);
    })) << id;
  }

  const auto signatures = GameSignatures();
  ASSERT_EQ(signatures.size(), 2);
  EXPECT_EQ(signatures[0].id, "a.match");
  EXPECT_EQ(signatures[0].pattern, "CC");
  EXPECT_EQ(signatures[0].target, SignatureTarget::kMatch);
  EXPECT_EQ(signatures[1].id, "b.call");
  EXPECT_EQ(signatures[1].modules, GameModule::kClient | GameModule::kEngine);
  EXPECT_EQ(signatures[1].target, SignatureTarget::kCall);
  EXPECT_EQ(FindGameSignature("pawn.respawn"), nullptr);
}

TEST(GameSignatureLoad, UnreadableFilesKeepTheSignatures) {
  ASSERT_TRUE(LoadGameSignatures(
      GameDataFile(R"(signatures { id: "kept" modules: GAME_MODULE_SERVER pattern: "CC" })")));
  const auto broken = LoadGameSignatures(GameDataFile("signatures {"));
  ASSERT_FALSE(broken);
  EXPECT_TRUE(broken.error().contains("line 1")) << broken.error();
  EXPECT_FALSE(
      LoadGameSignatures(std::filesystem::temp_directory_path() / "modlock-missing.txtpb"));
  EXPECT_NE(FindGameSignature("kept"), nullptr);
}

TEST(GameSignatureLoad, ShippedFileLoadsWhole) {
  const auto load = LoadGameSignatures(MODLOCK_GAME_SIGNATURES_FILE);
  ASSERT_TRUE(load) << load.error();
  EXPECT_TRUE(load->skipped.empty()) << load->skipped.front();
  EXPECT_EQ(load->loaded, GameSignatures().size());
  EXPECT_NE(FindGameSignature("pawn.respawn"), nullptr);
}

}  // namespace
