// Contract tests for the combat-event decoder seam: byte fixtures prove the
// damage, heal, shield and ability payloads decode into their documented fields, that invalid
// memory and impossible values surface as decode failures instead of
// zero-filled successes, and that an absent attacker is carried raw rather
// than fabricated. The hook owner itself is Windows-only; these tests run
// wherever the host test suite runs.
#include "modlock/gameinterop/combat_events.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

using modlock::gameinterop::BoundedReader;
using modlock::gameinterop::DamageResultOffsets;
using modlock::gameinterop::DecodeAbilityExecuted;
using modlock::gameinterop::DecodeDamageTaken;
using modlock::gameinterop::DecodeHealthTaken;

constexpr DamageResultOffsets kOffsets{8, 12, 16};

// RegionReader serves decoder reads out of named fixture regions, mirroring
// how the native payload references a separate result allocation. A read is
// valid only when the whole object lies inside a single region.
class RegionReader {
 public:
  explicit RegionReader(std::initializer_list<std::pair<const void*, size_t>> regions) {
    for (const auto& region : regions) {
      regions_.emplace_back(static_cast<const unsigned char*>(region.first), region.second);
    }
  }

  bool Read(const void* source, void* out, size_t bytes) const {
    for (const auto& region : regions_) {
      const auto* begin = region.first;
      const auto* from = static_cast<const unsigned char*>(source);
      if (from >= begin && from + bytes <= begin + region.second) {
        std::memcpy(out, from, bytes);
        return true;
      }
    }
    return false;
  }

 private:
  std::vector<std::pair<const unsigned char*, size_t>> regions_;
};

// MemoryReader serves reads from one contiguous buffer.
BoundedReader MemoryReader(const void* base, size_t size) {
  return [base, size](const void* source, void* out, size_t bytes) {
    const auto* begin = static_cast<const unsigned char*>(base);
    const auto* from = static_cast<const unsigned char*>(source);
    if (from < begin || from + bytes > begin + size) return false;
    std::memcpy(out, from, bytes);
    return true;
  };
}

// FailReader rejects every read: the decoder must surface that, not zeros.
BoundedReader FailReader() {
  return [](const void*, void*, size_t) { return false; };
}

TEST(DecodeDamageTaken, DecodesTheDocumentedLayout) {
  // Result struct: health_lost at +8, health_before at +12, damage_dealt at
  // +16 (the resolved schema offsets this test pins).
  unsigned char result[32] = {};
  const int32_t health_lost = 37;
  const int32_t health_before = 412;
  const int32_t damage_dealt = 45;
  std::memcpy(result + 8, &health_lost, sizeof(health_lost));
  std::memcpy(result + 12, &health_before, sizeof(health_before));
  std::memcpy(result + 16, &damage_dealt, sizeof(damage_dealt));

  unsigned char payload[28] = {};
  const uint32_t victim = 0x0123'4567;
  const uint32_t attacker = 0x89ab'cdef;
  std::memcpy(payload + 0, &victim, sizeof(victim));
  std::memcpy(payload + 4, &attacker, sizeof(attacker));
  const void* result_ptr = result;
  std::memcpy(payload + 16, &result_ptr, sizeof(result_ptr));

  RegionReader reader({{payload, sizeof(payload)}, {result, sizeof(result)}});
  auto event = DecodeDamageTaken(payload, kOffsets, [&reader](const void* s, void* o, size_t n) {
    return reader.Read(s, o, n);
  });
  ASSERT_TRUE(event.has_value()) << event.error();
  EXPECT_EQ(event->victim_handle, victim);
  EXPECT_EQ(event->attacker_handle, attacker);
  EXPECT_EQ(event->health_lost, health_lost);
  EXPECT_EQ(event->health_before, health_before);
  EXPECT_EQ(event->damage_dealt, damage_dealt);
}

TEST(DecodeDamageTaken, CarriesAnAbsentAttackerRawInsteadOfFabricatingOne) {
  unsigned char result[32] = {};
  const int32_t health_lost = 10;
  std::memcpy(result + 8, &health_lost, sizeof(health_lost));

  unsigned char payload[28] = {};
  const uint32_t victim = 0x0123'4567;
  const uint32_t absent_attacker = 0xffffffff;  // the game's invalid handle
  std::memcpy(payload + 0, &victim, sizeof(victim));
  std::memcpy(payload + 4, &absent_attacker, sizeof(absent_attacker));
  const void* result_ptr = result;
  std::memcpy(payload + 16, &result_ptr, sizeof(result_ptr));

  RegionReader reader({{payload, sizeof(payload)}, {result, sizeof(result)}});
  auto event = DecodeDamageTaken(payload, kOffsets, [&reader](const void* s, void* o, size_t n) {
    return reader.Read(s, o, n);
  });
  ASSERT_TRUE(event.has_value()) << event.error();
  EXPECT_EQ(event->attacker_handle, absent_attacker);
}

TEST(DecodeDamageTaken, SurfacesANullResultInsteadOfZeroSuccess) {
  unsigned char payload[28] = {};
  const void* result_ptr = nullptr;
  std::memcpy(payload + 16, &result_ptr, sizeof(result_ptr));

  auto event = DecodeDamageTaken(payload, kOffsets, MemoryReader(payload, sizeof(payload)));
  ASSERT_FALSE(event.has_value());
  EXPECT_NE(event.error().find("null"), std::string::npos) << event.error();
}

TEST(DecodeDamageTaken, SurfacesAFaultingResultPointerAsADecodeFailure) {
  unsigned char payload[28] = {};
  const void* result_ptr = reinterpret_cast<const void*>(0xdead'beef'0000'0010);
  std::memcpy(payload + 16, &result_ptr, sizeof(result_ptr));

  auto event = DecodeDamageTaken(payload, kOffsets, MemoryReader(payload, sizeof(payload)));
  ASSERT_FALSE(event.has_value());
  EXPECT_NE(event.error().find("cannot read"), std::string::npos) << event.error();
}

TEST(DecodeDamageTaken, SurfacesAnUnreadablePayloadInsteadOfZeroSuccess) {
  auto event = DecodeDamageTaken(nullptr, kOffsets, FailReader());
  ASSERT_FALSE(event.has_value());
  EXPECT_NE(event.error().find("null"), std::string::npos) << event.error();

  unsigned char payload[28] = {};
  auto unreadable = DecodeDamageTaken(payload, kOffsets, FailReader());
  ASSERT_FALSE(unreadable.has_value());
  EXPECT_NE(unreadable.error().find("cannot read"), std::string::npos) << unreadable.error();
}

TEST(DecodeHealthTaken, PreservesRequestedVersusAppliedForOverhealDiagnostics) {
  unsigned char payload[12] = {};
  const uint32_t entity = 0x0123'4567;
  const float requested = 150.0f;
  const int32_t applied = 40;  // the game clamped the rest away
  std::memcpy(payload + 0, &entity, sizeof(entity));
  std::memcpy(payload + 4, &requested, sizeof(requested));
  std::memcpy(payload + 8, &applied, sizeof(applied));

  auto event = DecodeHealthTaken(payload, MemoryReader(payload, sizeof(payload)));
  ASSERT_TRUE(event.has_value()) << event.error();
  EXPECT_EQ(event->entity_handle, entity);
  EXPECT_FLOAT_EQ(event->requested, requested);
  EXPECT_EQ(event->applied, applied);
}

TEST(DecodeHealthTaken, RejectsANonfiniteRequest) {
  unsigned char payload[12] = {};
  const float requested = 3.4e38f * 2.0f;  // overflow to infinity
  std::memcpy(payload + 4, &requested, sizeof(requested));

  auto event = DecodeHealthTaken(payload, MemoryReader(payload, sizeof(payload)));
  ASSERT_FALSE(event.has_value());
  EXPECT_NE(event.error().find("nonfinite"), std::string::npos) << event.error();
}

TEST(DecodeHealthTaken, RejectsANegativeAppliedValue) {
  unsigned char payload[12] = {};
  const int32_t applied = -5;
  std::memcpy(payload + 8, &applied, sizeof(applied));

  auto event = DecodeHealthTaken(payload, MemoryReader(payload, sizeof(payload)));
  ASSERT_FALSE(event.has_value());
  EXPECT_NE(event.error().find("negative"), std::string::npos) << event.error();
}

TEST(DecodeHealthTaken, SurfacesAnUnreadablePayloadInsteadOfZeroSuccess) {
  auto event = DecodeHealthTaken(nullptr, FailReader());
  ASSERT_FALSE(event.has_value());
  EXPECT_NE(event.error().find("null"), std::string::npos) << event.error();
}

TEST(DecodeShieldDamage, CreditsOwnerRatherThanShieldCasterAndPreservesFraction) {
  const uint32_t handles[] = {51, 72, 93};
  unsigned char payload[17] = {};
  std::memcpy(payload, handles, sizeof(handles));
  const float amount = 18.25f;
  std::memcpy(payload + 12, &amount, sizeof(amount));
  auto value =
      modlock::gameinterop::DecodeShieldDamage(payload, MemoryReader(payload, sizeof(payload)));
  ASSERT_TRUE(value) << value.error();
  EXPECT_EQ(value->attacker_handle, 51u);
  EXPECT_EQ(value->victim_handle, 72u);
  EXPECT_EQ(value->absorbed, amount);
  const float invalid = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(payload + 12, &invalid, sizeof(invalid));
  EXPECT_FALSE(
      modlock::gameinterop::DecodeShieldDamage(payload, MemoryReader(payload, sizeof(payload))));
  EXPECT_FALSE(modlock::gameinterop::DecodeShieldDamage(payload, MemoryReader(payload, 12)));
}

TEST(DecodeAbilityExecuted, PreservesCasterAbilityAndUntargetedExecution) {
  unsigned char payload[12] = {};
  const uint32_t caster = 0x0123'4567;
  const uint32_t ability = 0x89ab'cdef;
  const uint32_t target = 0x7654'3210;
  std::memcpy(payload, &caster, sizeof(caster));
  std::memcpy(payload + 4, &ability, sizeof(ability));
  std::memcpy(payload + 8, &target, sizeof(target));

  auto event = DecodeAbilityExecuted(payload, MemoryReader(payload, sizeof(payload)));
  ASSERT_TRUE(event) << event.error();
  EXPECT_EQ(event->caster_handle, caster);
  EXPECT_EQ(event->ability_handle, ability);
  EXPECT_EQ(event->target_handle, target);

  const uint32_t absent_target = 0xffffffff;
  std::memcpy(payload + 8, &absent_target, sizeof(absent_target));
  event = DecodeAbilityExecuted(payload, MemoryReader(payload, sizeof(payload)));
  ASSERT_TRUE(event) << event.error();
  EXPECT_EQ(event->target_handle, absent_target);
}

TEST(DecodeAbilityExecuted, RejectsMissingOrUnreadablePayload) {
  unsigned char payload[12] = {};
  EXPECT_FALSE(DecodeAbilityExecuted(nullptr, FailReader()));
  EXPECT_FALSE(DecodeAbilityExecuted(payload, {}));
  EXPECT_FALSE(DecodeAbilityExecuted(payload, FailReader()));
  for (size_t readable : {size_t{3}, size_t{7}, size_t{11}}) {
    auto event = DecodeAbilityExecuted(payload, MemoryReader(payload, readable));
    ASSERT_FALSE(event) << readable;
    EXPECT_NE(event.error().find("cannot read"), std::string::npos) << event.error();
  }
}

TEST(ProcessDamageContact, AdjustsNativeAmountAndPreservesSuppressionPriority) {
  using namespace modlock::gameinterop;
  struct Info {
    uint32_t attacker = 11;
    uint32_t ability = 23;
    uint64_t flags = kDamageHeavyMelee;
    float amount = 100;
    uint32_t inflictor = 0x12340027;
    int32_t hit_group = 1;
  } info;
  struct Payload {
    uint32_t victim = 12;
    uint32_t attacker = 11;
    Info* info;
  } payload{12, 11, &info};
  RegionReader regions({{&payload, sizeof(payload)}, {&info, sizeof(info)}});
  const auto read = [&](const void* source, void* out, size_t size) {
    return regions.Read(source, out, size);
  };
  const auto write = [&](void* target, const void* source, size_t size) {
    auto* bytes = static_cast<unsigned char*>(target);
    auto* begin = reinterpret_cast<unsigned char*>(&info);
    if (bytes < begin || bytes + size > begin + sizeof(info)) return false;
    std::memcpy(target, source, size);
    return true;
  };
  const DamageContactOffsets offsets{offsetof(Info, attacker),  offsetof(Info, flags),
                                     offsetof(Info, amount),    offsetof(Info, ability),
                                     offsetof(Info, inflictor), offsetof(Info, hit_group)};
  int calls = 0;
  const auto adjust = [&](const DamageContactEvent& event, float& amount) {
    EXPECT_EQ(event.attacker_handle, 11);
    EXPECT_EQ(event.victim_handle, 12);
    EXPECT_EQ(event.ability_handle, 23);
    EXPECT_EQ(event.inflictor_handle, 0x12340027);
    EXPECT_EQ(event.hit_group, 1);
    EXPECT_EQ(event.flags, kDamageHeavyMelee);
    ++calls;
    amount *= 2;
  };
  ASSERT_TRUE(ProcessDamageContact(&payload, offsets, read, write, {}, adjust));
  EXPECT_EQ(info.amount, 200);
  EXPECT_EQ(info.flags, kDamageHeavyMelee);
  ASSERT_TRUE(ProcessDamageContact(
      &payload, offsets, read, write, [](const auto&) { return true; }, adjust));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(info.amount, 200);
  EXPECT_NE(info.flags & 1, 0);
}

TEST(DecodeDamageTaken, RetainsSourceAbilityFromTheNativeDescriptor) {
  struct Info {
    uint32_t ability = 47;
  } info;
  struct Result {
    int32_t lost = 10, before = 100, dealt = 10;
  } result;
  struct Payload {
    uint32_t victim = 12, attacker = 11;
    Info* info;
    Result* result;
  } payload{12, 11, &info, &result};
  RegionReader regions(
      {{&payload, sizeof(payload)}, {&info, sizeof(info)}, {&result, sizeof(result)}});
  auto decoded = DecodeDamageTaken(
      &payload, {0, 4, 8, 0},
      [&](const void* source, void* out, size_t size) { return regions.Read(source, out, size); });
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(decoded->ability_handle, 47);
  EXPECT_EQ(decoded->health_lost, 10);
}

// EnumImage lays out enumerator records the way a module's schema tables do:
// NUL-terminated names, then 16-byte {name pointer, int64 value} records.
class EnumImage : public modlock::gameinterop::ModuleImage {
 public:
  static constexpr std::uintptr_t kBase = 0x180000000;

  void Add(const std::string& name, int64_t value) {
    const size_t at = bytes_.size() + 1;
    bytes_.push_back(0);
    bytes_.insert(bytes_.end(), name.begin(), name.end());
    bytes_.push_back(0);
    records_.push_back({kBase + at, value});
  }

  // Seal appends the aligned records after the names.
  void Seal() {
    while (bytes_.size() % 8 != 0) bytes_.push_back(0xCC);
    for (const auto& [pointer, value] : records_) {
      const size_t at = bytes_.size();
      bytes_.resize(at + 16);
      std::memcpy(bytes_.data() + at, &pointer, 8);
      std::memcpy(bytes_.data() + at + 8, &value, 8);
    }
  }

  std::uintptr_t base() const override { return kBase; }
  std::span<const uint8_t> image_bytes() const override { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
  std::vector<std::pair<uint64_t, int64_t>> records_;
};

TEST(ModifierEventIndex, ReadsTheRunningGamesValueByName) {
  EnumImage image;
  // A longer name sharing the prefix must not match.
  image.Add("MODIFIER_EVENT_MELEE_ATTACK_STARTED_LATE", 0x70);
  image.Add("MODIFIER_EVENT_MELEE_ATTACK_STARTED", 0x59);
  image.Add("MODIFIER_EVENT_DAMAGE_TAKEN", 0x18);
  image.Seal();
  auto melee = modlock::gameinterop::ModifierEventIndex(image, "MODIFIER_EVENT_MELEE_ATTACK_STARTED");
  ASSERT_TRUE(melee) << melee.error();
  EXPECT_EQ(*melee, 0x59u);
  auto damage = modlock::gameinterop::ModifierEventIndex(image, "MODIFIER_EVENT_DAMAGE_TAKEN");
  ASSERT_TRUE(damage) << damage.error();
  EXPECT_EQ(*damage, 0x18u);
  EXPECT_FALSE(modlock::gameinterop::ModifierEventIndex(image, "MODIFIER_EVENT_MISSING"));
}

TEST(ModifierEventIndex, RefusesANameWithoutARecord) {
  EnumImage image;
  image.Add("MODIFIER_EVENT_SLIDE", 0x3f);
  // The name exists but no record points at it.
  EnumImage bare;
  bare.Seal();
  image.Seal();
  EXPECT_TRUE(modlock::gameinterop::ModifierEventIndex(image, "MODIFIER_EVENT_SLIDE"));
  EXPECT_FALSE(modlock::gameinterop::ModifierEventIndex(bare, "MODIFIER_EVENT_SLIDE"));
}

}  // namespace
