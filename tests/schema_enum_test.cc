#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace modlock::gameinterop {
namespace {

// EnumSchema mirrors recorded x64 SchemaEnumInfoData_t and its hidden-return API.
struct EnumSchema {
  struct Interface {
    void** methods;
    void* result;
  };
  static void* Scope(void* self, const char*, const char**) {
    return static_cast<Interface*>(self)->result;
  }
  static void Enum(void* self, void* result, const char*) {
    *static_cast<void**>(result) = static_cast<Interface*>(self)->result;
  }

  EnumSchema() {
    // Route the type scope lookup to the fake enum.
    system_methods[FindVirtualSlot("schema-system.find-type-scope")->index] =
        reinterpret_cast<void*>(&Scope);
    scope_methods[3] = reinterpret_cast<void*>(&Enum);

    // Lay out two values and their names.
    const uint16_t count = 2;
    std::memcpy(info.data() + 28, &count, sizeof(count));
    auto* entries = values.data();
    std::memcpy(info.data() + 32, &entries, sizeof(entries));
    const std::array<const char*, 2> names{"MODIFIER_STATE_PARRY_DISABLED",
                                           "MODIFIER_STATE_PARRY_ACTIVE"};
    for (size_t i = 0; i < names.size(); ++i) {
      std::memcpy(values.data() + i * 32, &names[i], sizeof(names[i]));
    }

    // Set the first value.
    const int64_t disabled = 0x6c;
    std::memcpy(values.data() + 8, &disabled, sizeof(disabled));
  }

  std::array<void*, 16> system_methods{};
  std::array<void*, 4> scope_methods{};
  std::array<unsigned char, 72> info{};
  std::array<unsigned char, 64> values{};
  Interface scope{scope_methods.data(), info.data()};
  Interface system{system_methods.data(), &scope};
};

TEST(SchemaEnum, ReadsTheInstalledValueInsteadOfPinningAnOldModifierNumber) {
  // The installed value is read for each build.
  EnumSchema schema;
  for (const int64_t active : {0xdb, 0xdd, 0x123}) {
    std::memcpy(schema.values.data() + 40, &active, sizeof(active));
    const auto value = SchemaEnumValueOf(&schema.system, "server.dll", "EModifierState",
                                         "MODIFIER_STATE_PARRY_ACTIVE");
    ASSERT_TRUE(value) << value.error();
    EXPECT_EQ(*value, active);
  }

  // An unknown name is refused.
  const auto absent = SchemaEnumValueOf(&schema.system, "server.dll", "EModifierState", "MISSING");
  ASSERT_FALSE(absent);
  EXPECT_NE(absent.error().find("MISSING"), std::string::npos);

  // Without a scope, nothing is read.
  schema.scope.result = nullptr;
  EXPECT_FALSE(SchemaEnumValueOf(&schema.system, "server.dll", "EModifierState",
                                 "MODIFIER_STATE_PARRY_ACTIVE"));
}

}  // namespace
}  // namespace modlock::gameinterop
