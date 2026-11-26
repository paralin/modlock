#include "modlock/gameinterop/game_symbols.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "modlock/gameinterop/signature.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace {

using modlock::gameinterop::FindGameSignature;
using modlock::gameinterop::FindVirtualSlot;
using modlock::gameinterop::GameModule;
using modlock::gameinterop::GameSignature;
using modlock::gameinterop::GameSignatures;
using modlock::gameinterop::GameVirtualSlots;
using modlock::gameinterop::LoadGameSignatures;
using modlock::gameinterop::ModuleImage;
using modlock::gameinterop::ResolveModuleSignatures;
using modlock::gameinterop::ResolveSignature;
using modlock::gameinterop::SignatureTarget;

class Image final : public ModuleImage {
 public:
  std::uintptr_t base() const override { return 0x1000; }

  std::span<const uint8_t> image_bytes() const override { return bytes; }

  std::array<uint8_t, 32> bytes{};
};

// LoadedImage reports itself loaded, as a module mapped at base does.
class LoadedImage final : public ModuleImage {
 public:
  explicit LoadedImage(std::uintptr_t base, size_t size = 32) : bytes(size), base_(base) {}

  std::uintptr_t base() const override { return base_; }

  std::span<const uint8_t> image_bytes() const override {
    ++reads;
    return bytes;
  }

  mutable size_t reads = 0;

  bool loaded() const override { return true; }

  std::vector<uint8_t> bytes;

 private:
  std::uintptr_t base_;
};

GameSignature Match(std::string_view pattern) {
  return {.id = "target", .pattern = pattern};
}

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
    // Ids are sorted and unique, and each names a module and a shape.
    const auto& signature = signatures[i];
    if (i > 0) {
      EXPECT_LT(signatures[i - 1].id, signature.id) << "sort and dedupe the table";
    }
    EXPECT_NE(signature.modules, 0) << signature.id;
    EXPECT_FALSE(signature.shape.empty()) << signature.id;

    // The pattern parses, and a direct match has no delta.
    const auto parsed =
        modlock::gameinterop::ParseSignature(std::string(signature.id), signature.pattern);
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_FALSE(parsed->bytes.empty()) << signature.id;
    if (signature.target == SignatureTarget::kMatch) {
      EXPECT_EQ(signature.delta, 0) << signature.id;
    }
    EXPECT_EQ(FindGameSignature(signature.id), &signature);
  }
  EXPECT_EQ(FindGameSignature("not.recorded"), nullptr);
}

TEST(GameSignatures, ModulesNameTheirFiles) {
  // Each module names its file, and a signature may span modules.
  EXPECT_EQ(modlock::gameinterop::GameModuleFile(GameModule::kServer), "server.dll");
  EXPECT_EQ(modlock::gameinterop::GameModuleFile(GameModule::kEngine), "engine2.dll");
  const GameSignature shared{.modules = GameModule::kServer | GameModule::kEngine};
  EXPECT_TRUE(shared.In(GameModule::kEngine));
  const GameSignature server{.modules = static_cast<uint8_t>(GameModule::kServer)};
  EXPECT_FALSE(server.In(GameModule::kEngine));
}

TEST(GameSymbols, EveryTargetRequiresOneMatch) {
  // A pattern that matches once resolves.
  Image image;
  image.bytes[4] = 0xcc;
  auto symbol = ResolveSignature(image, Match("CC"));
  ASSERT_TRUE(symbol) << symbol.error();
  EXPECT_EQ(*symbol, reinterpret_cast<void*>(0x1004));

  // A pattern that matches twice, never or badly is refused for every target.
  image.bytes[16] = 0xcc;
  for (const auto pattern : {"CC", "FF", "", "ZZ"}) {
    EXPECT_FALSE(ResolveSignature(image, Match(pattern)));
    EXPECT_FALSE(ResolveSignature(image, Relative(pattern, SignatureTarget::kCall, 0)));
    EXPECT_FALSE(ResolveSignature(image, Relative(pattern, SignatureTarget::kRipRelative, 0)));
  }

  // An unrecorded id is refused by name.
  const auto unknown = ResolveSignature(image, "not.recorded");
  ASSERT_FALSE(unknown);
  EXPECT_NE(unknown.error().find("not.recorded"), std::string::npos);
}

TEST(GameSymbols, RelativeInstructionsPreserveSignedDisplacements) {
  for (const int32_t displacement : {-8, 16}) {
    // A call resolves relative to the next instruction.
    Image image;
    image.bytes[4] = 0xcc;
    image.bytes[5] = 0xe8;
    std::memcpy(image.bytes.data() + 6, &displacement, sizeof(displacement));
    auto call = ResolveSignature(image, Relative("CC E8", SignatureTarget::kCall, 1));
    ASSERT_TRUE(call) << call.error();
    EXPECT_EQ(*call, reinterpret_cast<void*>(0x100a + displacement));

    // So does a RIP-relative load.
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
  // A relative operand past the image or a wrapping delta is refused.
  Image image;
  image.bytes[4] = 0xcc;
  for (const size_t delta : {size_t{28}, std::numeric_limits<size_t>::max()}) {
    EXPECT_FALSE(ResolveSignature(image, Relative("CC", SignatureTarget::kCall, delta)));
    EXPECT_FALSE(ResolveSignature(image, Relative("CC", SignatureTarget::kRipRelative, delta)));
  }

  // An instruction cut off at the image's end is refused.
  image.bytes[30] = 0xe8;
  EXPECT_FALSE(ResolveSignature(image, Relative("E8", SignatureTarget::kCall, 0)));
  EXPECT_FALSE(ResolveSignature(image, Relative("E8", SignatureTarget::kRipRelative, 0)));
  EXPECT_FALSE(ResolveSignature(image, Relative("CC", SignatureTarget::kCall, 0)));
}

TEST(GameSymbols, RelativeInstructionsRejectOtherOpcodesAndOutsideTargets) {
  // A rip relative load must be a lea or mov the resolver knows.
  Image image;
  image.bytes[4] = 0xcc;
  image.bytes[5] = 0x41;
  image.bytes[6] = 0x8b;
  image.bytes[7] = 0xc5;
  EXPECT_FALSE(ResolveSignature(image, Relative("CC 41 8B C5", SignatureTarget::kRipRelative, 1)));

  // A target outside the image is refused; one inside resolves.
  image.bytes[5] = 0x4c;
  image.bytes[6] = 0x8d;
  image.bytes[7] = 0x0d;
  for (const int32_t displacement : {-13, 20}) {
    std::memcpy(image.bytes.data() + 8, &displacement, sizeof(displacement));
    EXPECT_FALSE(
        ResolveSignature(image, Relative("CC 4C 8D 0D", SignatureTarget::kRipRelative, 1)));
  }
  const int32_t inside = 19;
  std::memcpy(image.bytes.data() + 8, &inside, sizeof(inside));
  EXPECT_TRUE(ResolveSignature(image, Relative("CC 4C 8D 0D", SignatureTarget::kRipRelative, 1)));
}

// The load tests replace the recorded signatures, so they end by loading the
// shipped game data file.

TEST(GameSignatureLoad, BrokenEntriesAreSkippedAlone) {
  // Load a file with two good signatures, one good slot and six broken entries.
  const auto load = LoadGameSignatures(GameDataFile(R"(
signatures {
  id: "b.call"
  modules: GAME_MODULE_SERVER
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
slots { id: "s.kept" module: "a.dll" class_name: "CThing" index: 3 pattern: "CC" }
slots { id: "s.kept" module: "a.dll" class_name: "CThing" index: 4 pattern: "DD" }
slots { id: "s.no-class" module: "a.dll" index: 3 pattern: "CC" }
)"));
  ASSERT_TRUE(load) << load.error();
  EXPECT_EQ(load->loaded, 2);
  EXPECT_EQ(load->slots, 1);
  ASSERT_EQ(load->skipped.size(), 6);
  for (const auto id : {"a.match", "c.no-module", "d.bad-pattern", "s.kept", "s.no-class"}) {
    EXPECT_TRUE(std::ranges::any_of(load->skipped, [id](const auto& reason) {
      return reason.contains(id);
    })) << id;
  }

  // The good signatures are recorded in order.
  const auto signatures = GameSignatures();
  ASSERT_EQ(signatures.size(), 2);
  EXPECT_EQ(signatures[0].id, "a.match");
  EXPECT_EQ(signatures[0].pattern, "CC");
  EXPECT_EQ(signatures[0].target, SignatureTarget::kMatch);

  // The second spans two modules, and a missing id finds nothing.
  EXPECT_EQ(signatures[1].id, "b.call");
  EXPECT_EQ(signatures[1].modules, GameModule::kServer | GameModule::kEngine);
  EXPECT_EQ(signatures[1].target, SignatureTarget::kCall);
  EXPECT_EQ(FindGameSignature("pawn.respawn"), nullptr);

  // The good slot is recorded.
  ASSERT_EQ(GameVirtualSlots().size(), 1);
  const auto* slot = FindVirtualSlot("s.kept");
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->class_name, "CThing");
  EXPECT_EQ(slot->index, 3);
  EXPECT_EQ(slot->pattern, "CC");
}

TEST(GameSignatureLoad, UnreadableFilesKeepTheSignatures) {
  // A broken or missing file keeps the loaded signatures.
  ASSERT_TRUE(LoadGameSignatures(
      GameDataFile(R"(signatures { id: "kept" modules: GAME_MODULE_SERVER pattern: "CC" })")));
  const auto broken = LoadGameSignatures(GameDataFile("signatures {"));
  ASSERT_FALSE(broken);
  EXPECT_TRUE(broken.error().contains("line 1")) << broken.error();
  EXPECT_FALSE(
      LoadGameSignatures(std::filesystem::temp_directory_path() / "modlock-missing.txtpb"));
  EXPECT_NE(FindGameSignature("kept"), nullptr);
}

TEST(GameSignatureLoad, LoadedImagesKeepEachAddress) {
  // Record one signature for each module.
  ASSERT_TRUE(LoadGameSignatures(GameDataFile(R"(
signatures { id: "a.server" modules: GAME_MODULE_SERVER pattern: "CC" shape: "a match" }
signatures { id: "b.engine" modules: GAME_MODULE_ENGINE pattern: "DD" shape: "a match" }
)")));

  // A successful address survives a hook rewriting its matching bytes.
  LoadedImage loaded(0x2000);
  loaded.bytes[4] = 0xcc;
  const auto found = ResolveSignature(loaded, "a.server");
  ASSERT_TRUE(found) << found.error();
  EXPECT_EQ(*found, reinterpret_cast<void*>(0x2004));

  // The address is kept, so a hook that rewrites the entry does not lose it.
  loaded.bytes[4] = 0xe9;
  const auto kept = ResolveSignature(loaded, "a.server");
  ASSERT_TRUE(kept) << kept.error();
  EXPECT_EQ(*kept, reinterpret_cast<void*>(0x2004));

  // An image that is not loaded is scanned on every lookup.
  Image image;
  image.bytes[4] = 0xcc;
  EXPECT_TRUE(ResolveSignature(image, "a.server"));
  image.bytes[4] = 0xe9;
  EXPECT_FALSE(ResolveSignature(image, "a.server"));

  // Resolving a module's signatures keeps both hits and misses.
  LoadedImage missing(0x3000);
  const auto gaps = ResolveModuleSignatures(missing, GameModule::kServer);
  ASSERT_EQ(gaps.size(), 1);
  EXPECT_EQ(gaps.front().id, "a.server");
  EXPECT_NE(gaps.front().error.find("a.server"), std::string::npos);
  missing.bytes[4] = 0xcc;
  EXPECT_EQ(ResolveModuleSignatures(missing, GameModule::kServer).size(), 1);
  EXPECT_EQ(missing.reads, 1);

  // Cached misses never read the image, at either size.
  for (const size_t size : {size_t{32}, size_t{320}}) {
    // Resolve the miss once before repeating it.
    LoadedImage absent(0x10000 + size, size);
    const auto failure = ResolveSignature(absent, "a.server");
    ASSERT_FALSE(failure);
    for (int i = 0; i < 10; ++i) {
      const auto repeated = ResolveSignature(absent, "a.server");
      ASSERT_FALSE(repeated);
      EXPECT_EQ(repeated.error(), failure.error());
    }
    EXPECT_EQ(absent.reads, 1);
  }

  // A different loaded module gets its own resolution.
  LoadedImage module(0x4000);
  module.bytes[8] = 0xdd;
  module.bytes[4] = 0xcc;
  EXPECT_TRUE(ResolveModuleSignatures(module, GameModule::kServer).empty());

  // It keeps the module's addresses and leaves another module's alone.
  module.bytes[4] = 0xe9;
  module.bytes[8] = 0;
  EXPECT_TRUE(ResolveSignature(module, "a.server"));
  EXPECT_FALSE(ResolveSignature(module, "b.engine"));
}

TEST(GameSignatureLoad, ShippedFileLoadsWhole) {
  // The shipped file loads without a skip.
  const auto load = LoadGameSignatures(MODLOCK_GAME_SIGNATURES_FILE);
  ASSERT_TRUE(load) << load.error();
  EXPECT_TRUE(load->skipped.empty()) << load->skipped.front();
  EXPECT_EQ(load->loaded, GameSignatures().size());
  EXPECT_EQ(load->slots, GameVirtualSlots().size());
  EXPECT_NE(FindGameSignature("pawn.respawn"), nullptr);
  EXPECT_NE(FindVirtualSlot("engine-server.globals"), nullptr);
}

}  // namespace
