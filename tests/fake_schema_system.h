#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "gameinterop/schema_layout.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace modlock::test {

// MakeSchemaField builds one schema field of the declared type at offset.
inline gameinterop::schema::Field MakeSchemaField(const char* name,
                                                  const gameinterop::schema::DeclaredType& declared,
                                                  int32_t offset) {
  return gameinterop::schema::Field{
      .name = name,
      .type = reinterpret_cast<const gameinterop::schema::Type*>(&declared),
      .offset = offset};
}

// FakeSchemaSystem serves declared classes and enums for server.dll through
// ISchemaSystem's vtable ABI, so code under test runs the real schema lookups
// on every platform. A fixture fills classes and enums, then calls Seal; the
// fake must not move afterwards, since the infos point at themselves.
struct FakeSchemaSystem {
  struct Scope {
    void** vtable;
    FakeSchemaSystem* owner;
  };

  struct System {
    void** vtable;
    Scope* scope;
  };

  FakeSchemaSystem() {
    system_methods[gameinterop::FindVirtualSlot("schema-system.find-type-scope")->index] =
        reinterpret_cast<void*>(&FindScope);
    scope_methods[gameinterop::FindVirtualSlot("schema-type-scope.find-declared-class")->index] =
        reinterpret_cast<void*>(&FindClass);
    scope_methods[gameinterop::FindVirtualSlot("schema-type-scope.find-declared-enum")->index] =
        reinterpret_cast<void*>(&FindEnum);
  }

  FakeSchemaSystem(const FakeSchemaSystem&) = delete;
  FakeSchemaSystem& operator=(const FakeSchemaSystem&) = delete;

  // Seal points each declared class and enum at itself, as the game's do.
  void Seal() {
    for (auto& info : classes) {
      info.self = &info;
    }
    for (auto& info : enums) {
      info.self = &info;
    }
  }

  std::vector<gameinterop::schema::ClassInfo> classes;
  std::vector<gameinterop::schema::EnumInfo> enums;
  std::array<void*, 16> system_methods{};
  std::array<void*, 16> scope_methods{};
  Scope scope{scope_methods.data(), this};
  System system{system_methods.data(), &scope};

 private:
  static void* FindScope(void* self, const char* module, const char**) {
    return std::string_view(module) == "server.dll" ? static_cast<System*>(self)->scope : nullptr;
  }

  static void FindClass(void* self, void* out, const char* name) {
    *static_cast<const void**>(out) = Find(static_cast<Scope*>(self)->owner->classes, name);
  }

  static void FindEnum(void* self, void* out, const char* name) {
    *static_cast<const void**>(out) = Find(static_cast<Scope*>(self)->owner->enums, name);
  }

  template <typename Info>
  static const Info* Find(const std::vector<Info>& infos, std::string_view name) {
    for (const auto& info : infos) {
      if (std::string_view(info.name) == name) {
        return &info;
      }
    }
    return nullptr;
  }
};

}  // namespace modlock::test
