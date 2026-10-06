// Contract tests for the signature scanner: pattern parsing, exact match
// offsets, zero-hit scans, and boundary-spanning candidates, all against a
// committed synthetic .text fixture. No process access anywhere.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gameinterop_fixtures.h"
#include "gtest/gtest.h"
#include "modlock/gameinterop/signature.h"

namespace {

using modlock::gameinterop::ParseSignature;
using modlock::gameinterop::Signature;
using modlock::gameinterop::SignatureScan;

// Verified layout (index: byte):
//   0:90  1:48  2:8b  3:05  4:11  5:22  6:33  7:44
//   8:cc  9:74  10:48 11:8b 12:05 13:aa 14:bb 15:cc 16:dd
constexpr uint8_t kText[] = {
    0x90, 0x48, 0x8b, 0x05, 0x11, 0x22, 0x33, 0x44, 0xcc,
    0x74, 0x48, 0x8b, 0x05, 0xaa, 0xbb, 0xcc, 0xdd,
};

Signature ParseOrDie(std::string id, std::string_view pattern) {
  auto sig = ParseSignature(std::move(id), pattern);
  if (!sig) {
    ADD_FAILURE() << "ParseSignature failed: " << sig.error();
    return Signature{};
  }
  return *std::move(sig);
}

TEST(SignatureTest, ParseConcreteAndWildcardTokens) {
  const auto sig = ParseOrDie("test", "48 8B ?? ?? 74");
  EXPECT_EQ(sig.id, "test");
  ASSERT_EQ(sig.bytes.size(), 5u);
  EXPECT_EQ(sig.bytes[0], 0x48);
  EXPECT_EQ(sig.bytes[1], 0x8b);
  EXPECT_EQ(sig.bytes[2], 0x00);
  EXPECT_FALSE(sig.wildcard[0]);
  EXPECT_TRUE(sig.wildcard[2]);
  EXPECT_TRUE(sig.wildcard[3]);
  EXPECT_FALSE(sig.wildcard[4]);
}

TEST(SignatureTest, ParseRejectsMalformedPatterns) {
  // A run of question marks represents one wildcard byte. Only
  // empty patterns and non-hex garbage must fail.
  for (const std::string bad : {"", "4", "zz", "48 8g"}) {
    auto sig = ParseSignature("bad", bad);
    EXPECT_FALSE(sig.has_value()) << "pattern: '" << bad << "'";
  }

  // A maximal run of question marks collapses to one wildcard byte.
  const auto wildcards = ParseOrDie("wild", "???");
  ASSERT_EQ(wildcards.bytes.size(), 1u);
  EXPECT_TRUE(wildcards.wildcard[0]);
}

TEST(SignatureTest, SingleHitAtPinnedOffset) {
  const auto sig = ParseOrDie("single", "48 8B 05 11 22");
  const auto hits = SignatureScan({kText, sizeof(kText)}, sig);
  EXPECT_EQ(hits, (std::vector<size_t>{1}));
}

TEST(SignatureTest, WildcardsMatchAnythingButPinNeighbors) {
  // Leading and interior wildcards; the trailing 22 pins the single hit
  // at offset 1 (data: 48 8B 05 11 22).
  const auto sig = ParseOrDie("wild", "?? 8B 05 ?? 22");
  const auto hits = SignatureScan({kText, sizeof(kText)}, sig);
  EXPECT_EQ(hits, (std::vector<size_t>{1}));
}

TEST(SignatureTest, ZeroHitsWhenPatternAbsent) {
  const auto sig = ParseOrDie("absent", "00 00 00");
  const auto hits = SignatureScan({kText, sizeof(kText)}, sig);
  EXPECT_TRUE(hits.empty());
}

TEST(SignatureTest, BoundarySpanningCandidateDoesNotMatch) {
  // A candidate starting at 12 ("05 AA BB CC DD") fits the blob exactly;
  // asking for one more byte ("... DD 90") would read past the end and must
  // never count as a hit.
  const auto truncated = ParseOrDie("tail", "05 AA BB CC DD 90");
  const auto hits = SignatureScan({kText, sizeof(kText)}, truncated);
  EXPECT_TRUE(hits.empty());

  const auto shorter = ParseOrDie("tail-short", "05 AA BB CC DD");
  EXPECT_EQ(SignatureScan({kText, sizeof(kText)}, shorter), (std::vector<size_t>{12}));
}

TEST(SignatureTest, FullTailSequenceMatchesAtItsOffset) {
  const auto sig = ParseOrDie("tail-full", "CC 74 48 8B");
  EXPECT_EQ(SignatureScan({kText, sizeof(kText)}, sig), (std::vector<size_t>{8}));
}

TEST(SignatureTest, MultipleHitsAllReported) {
  const auto sig = ParseOrDie("multi", "48 8B");
  const auto hits = SignatureScan({kText, sizeof(kText)}, sig);
  EXPECT_EQ(hits, (std::vector<size_t>{1, 10}));
}

// Recorded game bytes: the scanner must find each recorded
// signature centered in its captured window. These pin the scanner against
// real server.dll bytes, not just synthetic blobs.
TEST(SignatureTest, RecordedGameWindowsMatchAtCenter) {
  {
    const auto sig = ParseOrDie("UTIL_Remove", "48 85 C9 74 ? 48 8B D1 48 8B 0D");
    EXPECT_EQ(SignatureScan(modlock::gameinterop::fixtures::kUtilRemoveWindow, sig),
              (std::vector<size_t>{128}));
  }
  {
    const auto sig = ParseOrDie("CBaseEntity::TakeDamageOld",
                                "40 55 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? "
                                "48 8D 6C 24 ?? 48 89 9D ?? ?? ?? ?? 45 33 ED");
    EXPECT_EQ(SignatureScan(modlock::gameinterop::fixtures::kTakeDamageOldWindow, sig),
              (std::vector<size_t>{128}));
  }
}

}  // namespace
