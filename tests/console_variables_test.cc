#include "modlock/gameinterop/console_variables.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"

namespace {

using modlock::gameinterop::ConsoleVariables;

struct Fixture {
  // Recorded x64 CCvar storage offsets, with sparse live entries 2 -> 0.
  // Slot 1 is not linked and must never be examined. The command registry
  // holds one inline entry.
  std::array<unsigned char, 0x118> registry{};
  std::array<unsigned char, 48> entries{};
  std::array<unsigned char, 0x38> commands{};
  float default_value = 0.25f;
  std::array<unsigned char, 0x60> data{};
  std::array<unsigned char, 0x60> unrelated{};

  void Link(size_t index, unsigned char* value, uint16_t previous, uint16_t next) {
    auto* entry = entries.data() + index * 16;
    std::memcpy(entry, &value, sizeof(value));
    std::memcpy(entry + 8, &previous, sizeof(previous));
    std::memcpy(entry + 10, &next, sizeof(next));
  }

  Fixture() {
    const char* name = "citadel_trooper_spawn_enabled";
    std::memcpy(data.data(), &name, sizeof(name));
    const uint64_t flags = 0x100004012;
    std::memcpy(data.data() + 0x30, &flags, sizeof(flags));
    data[0x58] = 1;
    unrelated = data;
    const char* other = "other";
    std::memcpy(unrelated.data(), &other, sizeof(other));
    registry[0x4a] = 3;
    registry[0x43] = 0x80;  // External-buffer bit is not capacity.
    registry[0x58] = 2;
    auto* base = entries.data();
    std::memcpy(registry.data() + 0x50, &base, sizeof(base));
    Link(2, unrelated.data(), 0xffff, 0);
    Link(0, data.data(), 2, 0xffff);

    const char* command = "changelevel";
    const char* help = "Change the map.";
    const uint64_t command_flags = 1ull << 1;
    std::memcpy(commands.data(), &command, sizeof(command));
    std::memcpy(commands.data() + 8, &help, sizeof(help));
    std::memcpy(commands.data() + 0x10, &command_flags, sizeof(command_flags));
    const uint16_t none = 0xffff;
    std::memcpy(commands.data() + 0x30, &none, sizeof(none));
    std::memcpy(commands.data() + 0x32, &none, sizeof(none));
    registry[0x102] = 1;
    auto* command_base = commands.data();
    std::memcpy(registry.data() + 0x108, &command_base, sizeof(command_base));
  }
};

TEST(ConsoleVariables, NamedExposureAndBooleanReadbackRefuseMissingOrWrongData) {
  Fixture fixture;
  auto variables = ConsoleVariables::Bind(fixture.registry.data());
  ASSERT_TRUE(variables);
  const auto untouched = fixture.unrelated;
  auto value = variables->ReadBool("citadel_trooper_spawn_enabled");
  ASSERT_TRUE(value);
  EXPECT_TRUE(*value);
  ASSERT_TRUE(variables->Expose("citadel_trooper_spawn_enabled"));
  uint64_t flags;
  std::memcpy(&flags, fixture.data.data() + 0x30, sizeof(flags));
  EXPECT_EQ(flags, 0x4000u);
  EXPECT_EQ(fixture.unrelated, untouched);
  // Exposure does not set the value; the engine parser owns that operation.
  EXPECT_EQ(fixture.data[0x58], 1);
  fixture.data[0x58] = 0;
  value = variables->ReadBool("citadel_trooper_spawn_enabled");
  ASSERT_TRUE(value);
  EXPECT_FALSE(*value);

  auto missing = variables->ReadBool("missing");
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.error().find("not found"), std::string::npos);
  EXPECT_FALSE(variables->Expose("missing"));
  EXPECT_FALSE(variables->Expose(nullptr));

  const int16_t integer_type = 3;
  std::memcpy(fixture.data.data() + 0x28, &integer_type, sizeof(integer_type));
  auto wrong_type = variables->ReadBool("citadel_trooper_spawn_enabled");
  ASSERT_FALSE(wrong_type);
  EXPECT_NE(wrong_type.error().find("not Boolean"), std::string::npos);

  const char* other_name = "other";
  std::memcpy(fixture.data.data(), &other_name, sizeof(other_name));
  const auto before = fixture.data;
  EXPECT_FALSE(variables->Expose("citadel_trooper_spawn_enabled"));
  EXPECT_EQ(fixture.data, before);
  EXPECT_FALSE(ConsoleVariables::Bind(nullptr));
  // Refuse an out-of-allocation next index before accessing its entry.
  fixture.Link(2, fixture.unrelated.data(), 0xffff, 3);
  EXPECT_FALSE(variables->Expose("missing"));
  // A free-list row or cycle cannot be mistaken for a live traversal.
  fixture.Link(2, fixture.unrelated.data(), 0xffff, 2);
  EXPECT_FALSE(variables->Expose("missing"));
  fixture.Link(2, fixture.unrelated.data(), 2, 0);
  EXPECT_FALSE(variables->Expose("missing"));
  EXPECT_EQ(fixture.data, before);
}

TEST(ConsoleVariables, ServerChangesPreserveReplicationAndOtherVariables) {
  Fixture fixture;
  auto variables = ConsoleVariables::Bind(fixture.registry.data());
  ASSERT_TRUE(variables);
  const auto untouched = fixture.unrelated;
  const uint64_t original = 0x100004012 | (1ull << 13);
  std::memcpy(fixture.data.data() + 0x30, &original, sizeof(original));
  ASSERT_TRUE(variables->AllowServerChanges("citadel_trooper_spawn_enabled"));
  uint64_t flags;
  std::memcpy(&flags, fixture.data.data() + 0x30, sizeof(flags));
  EXPECT_EQ(flags, 1ull << 13);
  EXPECT_EQ(fixture.unrelated, untouched);
  EXPECT_EQ(fixture.data[0x58], 1);
  EXPECT_FALSE(variables->AllowServerChanges("missing"));
}

TEST(ConsoleVariables, TypedWritesRefuseOtherTypes) {
  Fixture fixture;
  auto variables = ConsoleVariables::Bind(fixture.registry.data());
  ASSERT_TRUE(variables);
  const auto untouched = fixture.unrelated;
  ASSERT_TRUE(variables->SetBool("citadel_trooper_spawn_enabled", false));
  EXPECT_EQ(fixture.data[0x58], 0);
  EXPECT_FALSE(variables->SetFloat("citadel_trooper_spawn_enabled", 1.5f));
  EXPECT_FALSE(variables->SetInt32("citadel_trooper_spawn_enabled", 2));
  const int16_t float_type = 7;
  std::memcpy(fixture.data.data() + 0x28, &float_type, sizeof(float_type));
  ASSERT_TRUE(variables->SetFloat("citadel_trooper_spawn_enabled", 57.5f));
  float value;
  std::memcpy(&value, fixture.data.data() + 0x58, sizeof(value));
  EXPECT_FLOAT_EQ(value, 57.5f);
  EXPECT_FLOAT_EQ(*variables->ReadFloat("citadel_trooper_spawn_enabled"), 57.5f);
  EXPECT_FALSE(variables->ReadBool("citadel_trooper_spawn_enabled"));
  EXPECT_FALSE(variables->SetBool("citadel_trooper_spawn_enabled", true));
  const int16_t integer_type = 3;
  std::memcpy(fixture.data.data() + 0x28, &integer_type, sizeof(integer_type));
  ASSERT_TRUE(variables->SetInt32("citadel_trooper_spawn_enabled", -2));
  int32_t integer;
  std::memcpy(&integer, fixture.data.data() + 0x58, sizeof(integer));
  EXPECT_EQ(integer, -2);
  EXPECT_FALSE(variables->SetFloat("missing", 1));
  EXPECT_FALSE(variables->ReadFloat("citadel_trooper_spawn_enabled"));
  EXPECT_FALSE(variables->ReadFloat("missing"));
  EXPECT_EQ(fixture.unrelated, untouched);
}

TEST(ConsoleVariables, ListingIncludesDevelopmentEntriesWithFormattedValues) {
  Fixture fixture;
  auto variables = ConsoleVariables::Bind(fixture.registry.data());
  ASSERT_TRUE(variables);
  const int16_t float_type = 7;
  std::memcpy(fixture.data.data() + 0x28, &float_type, sizeof(float_type));
  const auto* default_value = &fixture.default_value;
  std::memcpy(fixture.data.data() + 0x08, &default_value, sizeof(default_value));

  auto listed = variables->Variables();
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 2u);
  EXPECT_EQ((*listed)[0].name, "other");
  const auto& variable = (*listed)[1];
  EXPECT_EQ(variable.name, "citadel_trooper_spawn_enabled");
  EXPECT_EQ(variable.type, 7);
  EXPECT_EQ(variable.flags, 0x100004012u);
  EXPECT_EQ(variable.default_value, "0.25");
  EXPECT_EQ(variable.min, "");

  auto commands = variables->Commands();
  ASSERT_TRUE(commands);
  ASSERT_EQ(commands->size(), 1u);
  EXPECT_EQ((*commands)[0].name, "changelevel");
  EXPECT_EQ((*commands)[0].help, "Change the map.");
  EXPECT_EQ((*commands)[0].flags, 1ull << 1);
}

}  // namespace
