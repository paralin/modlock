#include "modlock/gameinterop/entity_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "modlock/gameinterop/mapped_module_image.h"
#include "schema_layout.h"

namespace modlock::gameinterop {
namespace {

// An entity reaches its schema class through CEntityIdentity::m_pClass,
// CEntityClass::m_pClassInfo and CEntityClassInfo::m_pSchemaBinding
// (sourcesdk public/entity2/entityidentity.h and entityclass.h).
constexpr size_t kIdentityClass = 0x08;
constexpr size_t kEntityClassInfo = 0x58;
constexpr size_t kClassInfoSchemaBinding = 0x28;

// CEntityInstance::NetworkStateChanged follows RequiredEdictIndex in the
// current Windows ABI. Slot 27 writes an entity index to its hidden result;
// calling it with a change record silently discards the notification.
constexpr size_t kNetworkStateChangedSlot = 28;

// kTeleportSlot is CBaseEntity::Teleport in the native entity dispatch table.
constexpr size_t kTeleportSlot = 163;

// Teleport calls CBaseEntity::Teleport. Null pose pointers preserve placement.
void Teleport(void* entity, const float* position, const float* angles, const float* velocity) {
  if (entity == nullptr) return;
  using TeleportFn = void (*)(void*, const float*, const float*, const float*);
  auto** table = *static_cast<void***>(entity);
  reinterpret_cast<TeleportFn>(table[kTeleportSlot])(entity, position, angles, velocity);
}

// NetworkStateNotificationOf resolves the shared replication entry for preflight
// and publication, so callers never maintain a second copy of its ABI slot.
void* NetworkStateNotificationOf(void* entity) {
  if (!entity) return nullptr;
  auto* table = *static_cast<void***>(entity);
  return table ? table[kNetworkStateChangedSlot] : nullptr;
}

// ReadPointer loads one aligned pointer from a live game object.
void* ReadPointer(const void* base, size_t offset) {
  void* value = nullptr;
  std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
  return value;
}

// VtableCall dispatches one recorded vtable slot on instance.
template <typename Fn>
Fn VtableCall(void* instance, size_t slot) {
  void* vtable = ReadPointer(instance, 0);
  return reinterpret_cast<Fn>(static_cast<void**>(vtable)[slot]);
}

// ReadLink copies the pointer at base + offset through read, or returns null
// when read fails or base is null.
const void* ReadLink(const void* base, size_t offset, const BoundedReader& read) {
  const void* value = nullptr;
  if (base == nullptr || !read(static_cast<const uint8_t*>(base) + offset, &value, sizeof(value)))
    return nullptr;
  return value;
}

// SchemaClassInfoOf borrows the declared class from the module's type scope.
std::expected<const schema::ClassInfo*, std::string> SchemaClassInfoOf(void* schema_system,
                                                                       const char* module_name,
                                                                       const char* class_name) {
  auto scope = FindTypeScopeOf(schema_system, module_name);
  if (!scope) return std::unexpected(scope.error());
  auto info = FindDeclaredClassOf(*scope, class_name);
  if (!info) return std::unexpected(info.error());
  return static_cast<const schema::ClassInfo*>(*info);
}

// IsFieldNetworked reports whether the field may be networked. Game build
// 6711 no longer ships MNetworkEnable metadata in the server schema, so a
// field counts as networked unless it carries MNetworkDisable; notifying an
// unnetworked field is harmless.
bool IsFieldNetworked(const schema::Field& field) {
  for (int32_t i = 0; i < field.metadata_count; ++i) {
    const char* name = field.metadata[i].name;
    if (name != nullptr && std::strcmp(name, "MNetworkDisable") == 0) return false;
  }
  return true;
}

// FirstBaseOf returns the first base class of a class info, or null for a
// root class. A first base shares the derived class's field offsets.
const schema::ClassInfo* FirstBaseOf(const schema::ClassInfo* info) {
  return info->base_count == 0 ? nullptr : info->bases[0].info;
}

// FieldInClassInfo searches one class info's own fields, then its first base
// class (single inheritance). A field's storage runs to the next field or the
// end of its class.
std::expected<SchemaField, std::string> FieldInClassInfo(const schema::ClassInfo* info,
                                                         const char* field_name) {
  for (; info != nullptr; info = FirstBaseOf(info)) {
    const std::span fields(info->fields, info->field_count);
    for (const auto& field : fields) {
      if (field.name == nullptr || std::strcmp(field.name, field_name) != 0) continue;
      int32_t boundary = info->size;
      for (const auto& next : fields) {
        if (next.offset > field.offset && (boundary <= field.offset || next.offset < boundary))
          boundary = next.offset;
      }
      if (field.offset < 0 || boundary <= field.offset)
        return std::unexpected("schema field storage span is invalid");
      return SchemaField{static_cast<size_t>(field.offset),
                         static_cast<size_t>(boundary - field.offset), IsFieldNetworked(field)};
    }
  }
  return std::unexpected("schema field '" + std::string(field_name) +
                         "' is not declared on the class chain");
}

}  // namespace

bool InteropTraceEnabled() {
  const char* value = std::getenv("MODLOCK_INTEROP_TRACE");
  return value != nullptr && value[0] == '1';
}

void TeleportEntity(void* entity, const std::array<float, 3>& position,
                    const std::array<float, 3>& angles, const std::array<float, 3>& velocity) {
  Teleport(entity, position.data(), angles.data(), velocity.data());
}

void TeleportEntity(void* entity, float x, float y, float z) {
  TeleportEntity(entity, {x, y, z}, {}, {});
}

void TeleportEntity(void* entity, const float* position, const float* angles,
                    const float* velocity) {
  Teleport(entity, position, angles, velocity);
}

void SetEntityVelocity(void* entity, const std::array<float, 3>& velocity) {
  Teleport(entity, nullptr, nullptr, velocity.data());
}

void* IdentityOf(void* entity) {
  return entity == nullptr ? nullptr : ReadPointer(entity, kIdentityOffset);
}

std::optional<uint32_t> ReferenceHandleOf(void* entity) {
  auto* identity = static_cast<const uint8_t*>(IdentityOf(entity));
  if (identity == nullptr || ReadPointer(identity, 0) != entity) {
    return std::nullopt;
  }
  uint32_t stored = 0;
  uint32_t flags = 0;
  std::memcpy(&stored, identity + 0x10, sizeof(stored));
  std::memcpy(&flags, identity + 0x30, sizeof(flags));
  constexpr uint32_t kDeleteInProgress = 0x10;
  constexpr uint32_t kMarkedForDelete = 0x200;
  if (stored == 0xFFFFFFFFu || (flags & (kDeleteInProgress | kMarkedForDelete)) != 0) {
    return std::nullopt;
  }
  constexpr uint32_t kIndexMask = 0x7FFF;
  constexpr uint32_t kSerialMask = 0x1FFFF;
  constexpr uint32_t kSerialShift = 15;
  constexpr uint32_t kInvalidHandleFlag = 0x1;
  const uint32_t serial = ((stored >> kSerialShift) - (flags & kInvalidHandleFlag)) & kSerialMask;
  return (stored & kIndexMask) | (serial << kSerialShift);
}

std::expected<const void*, std::string> SchemaClassOfEntity(void* entity,
                                                            const BoundedReader& read) {
  const void* identity = ReadLink(entity, kIdentityOffset, read);
  if (identity == nullptr || ReadLink(identity, 0, read) != entity)
    return std::unexpected("the entity has no live identity");
  const void* binding =
      ReadLink(ReadLink(ReadLink(identity, kIdentityClass, read), kEntityClassInfo, read),
               kClassInfoSchemaBinding, read);
  if (binding == nullptr || ReadLink(binding, 0, read) != binding)
    return std::unexpected("the entity's schema class is unreadable");
  return binding;
}

std::string_view SchemaClassNameOf(const void* class_info) {
  const char* name = static_cast<const schema::ClassInfo*>(class_info)->name;
  return name == nullptr ? std::string_view() : std::string_view(name);
}

bool SchemaClassDerivesFrom(const void* class_info, std::string_view class_name) {
  for (auto* info = static_cast<const schema::ClassInfo*>(class_info); info != nullptr;
       info = FirstBaseOf(info)) {
    if (SchemaClassNameOf(info) == class_name) return true;
  }
  return false;
}

std::expected<void*, std::string> EntitySystemFromResourceService(void* resource_service) {
  if (resource_service == nullptr) {
    return std::unexpected("the resource service instance is not resolved yet");
  }
  return ReadPointer(resource_service, kResourceServiceEntitySystemOffset);
}

std::vector<void*> EntityInstances(void* entity_system) {
  std::vector<void*> instances;
  if (!entity_system) return instances;
  // CConcreteEntityList follows the vtable and current resource manifest.
  // Each of its 64 chunks contains 512 CEntityIdentity records of 0x70 bytes.
  const auto* chunks = static_cast<const uint8_t*>(entity_system) + 0x10;
  for (uint32_t chunk = 0; chunk < 64; ++chunk) {
    const auto* entries = static_cast<const uint8_t*>(ReadPointer(chunks, chunk * sizeof(void*)));
    if (!entries) continue;
    for (uint32_t index = 0; index < 512; ++index) {
      const auto* identity = entries + index * 0x70;
      void* instance = ReadPointer(identity, 0);
      if (!instance || IdentityOf(instance) != identity) continue;
      auto handle = ReferenceHandleOf(instance);
      if (handle && (*handle & 0x7FFF) == chunk * 512 + index) instances.push_back(instance);
    }
  }
  return instances;
}

void* EntityInstance(void* entity_system, uint32_t handle) {
  if (!entity_system || handle == 0xFFFFFFFFu) return nullptr;
  const uint32_t index = handle & 0x7FFF;
  const auto* chunks = static_cast<const uint8_t*>(entity_system) + 0x10;
  const auto* chunk =
      static_cast<const uint8_t*>(ReadPointer(chunks, (index / 512) * sizeof(void*)));
  if (!chunk) return nullptr;
  const auto* identity = chunk + (index % 512) * 0x70;
  void* instance = ReadPointer(identity, 0);
  if (!instance || IdentityOf(instance) != identity || ReferenceHandleOf(instance) != handle)
    return nullptr;
  return instance;
}

std::expected<void*, std::string> ResolveLiveEntitySystem() {
  const auto resource_service =
      ResolveEngineInterface(L"engine2.dll", kGameResourceServiceServerVersion);
  if (!resource_service) return std::unexpected(resource_service.error());

  // Print the borrowed service once when diagnosing a shifted native layout.
  static const bool trace = InteropTraceEnabled();
  if (trace) {
    static bool traced = false;
    if (!traced) {
      traced = true;
      const auto* service = static_cast<const unsigned char*>(*resource_service);
      std::fprintf(stderr, "[modlock] interop trace: resource_service=%p\n", *resource_service);
      for (size_t offset = 0; offset < 0x100; offset += sizeof(void*)) {
        void* value = nullptr;
        std::memcpy(&value, service + offset, sizeof(value));
        std::fprintf(stderr, "[modlock] interop trace: service+0x%02zx = %p\n", offset, value);
      }
      std::fflush(stderr);
    }
  }
  return EntitySystemFromResourceService(*resource_service);
}

std::expected<void*, std::string> ResolveSchemaSystem() {
  return ResolveEngineInterface(L"schemasystem.dll", "SchemaSystem_001");
}

std::expected<void*, std::string> FindTypeScopeOf(void* schema_system, const char* module_name) {
  if (!schema_system) return std::unexpected("the schema system instance is not resolved yet");
  using FindTypeScope = void* (*)(void*, const char*, const char**);
  auto* scope = VtableCall<FindTypeScope>(schema_system, kSchemaSystemFindTypeScopeSlot)(
      schema_system, module_name, nullptr);
  if (!scope)
    return std::unexpected("schema type scope '" + std::string(module_name) + "' is not open");
  return scope;
}

namespace {

// FindDeclared calls a type scope lookup with the hidden-return ABI described
// at FindDeclaredClassOf in the header.
std::expected<void*, std::string> FindDeclared(void* type_scope, size_t slot, const char* kind,
                                               const char* name) {
  if (type_scope == nullptr) {
    return std::unexpected("the schema type scope instance is not resolved yet");
  }
  using FindFn = void (*)(void* type_scope, void* out_handle, const char* name);
  void* info = nullptr;
  VtableCall<FindFn>(type_scope, slot)(type_scope, &info, name);
  if (info == nullptr) {
    return std::unexpected(std::string("schema ") + kind + " '" + name + "' is not declared");
  }
  return info;
}

}  // namespace

std::expected<void*, std::string> FindDeclaredClassOf(void* type_scope, const char* class_name) {
  return FindDeclared(type_scope, kTypeScopeFindDeclaredClassSlot, "class", class_name);
}

std::expected<void*, std::string> FindDeclaredEnumOf(void* type_scope, const char* enum_name) {
  return FindDeclared(type_scope, kTypeScopeFindDeclaredEnumSlot, "enum", enum_name);
}

std::expected<SchemaField, std::string> SchemaFieldOf(void* schema_system, const char* module_name,
                                                      const char* class_name,
                                                      const char* field_name) {
  auto info = SchemaClassInfoOf(schema_system, module_name, class_name);
  if (!info) return std::unexpected(info.error());
  return FieldInClassInfo(*info, field_name);
}

std::expected<size_t, std::string> SchemaClassSizeOf(void* schema_system, const char* module_name,
                                                     const char* class_name) {
  auto info = SchemaClassInfoOf(schema_system, module_name, class_name);
  if (!info) return std::unexpected(info.error());
  if ((*info)->size <= 0) return std::unexpected("schema class allocation size is invalid");
  return static_cast<size_t>((*info)->size);
}

std::expected<int64_t, std::string> SchemaEnumValueOf(void* schema_system, const char* module_name,
                                                      const char* enum_name,
                                                      const char* value_name) {
  auto scope = FindTypeScopeOf(schema_system, module_name);
  if (!scope) return std::unexpected(scope.error());
  auto found = FindDeclaredEnumOf(*scope, enum_name);
  if (!found) return std::unexpected(found.error());
  const auto* info = static_cast<const schema::EnumInfo*>(*found);
  for (const auto& enumerator : std::span(info->enumerators, info->enumerator_count)) {
    if (std::strcmp(enumerator.name, value_name) == 0) return enumerator.value;
  }
  return std::unexpected("schema enum value '" + std::string(enum_name) + "." + value_name +
                         "' is not declared");
}

bool CanNotifyEntityStateChanged(void* entity) {
  return NetworkStateNotificationOf(entity) != nullptr;
}

bool NotifyEntityStateChanged(void* entity) {
  auto* notification = NetworkStateNotificationOf(entity);
  if (!notification) return false;
  // Windows x64 NetworkStateChanged_t full-change form.
  struct alignas(8) FullChange {
    uint32_t flag = 0;
    uint32_t padding = 0;
    std::byte offsets_and_names[40]{};
    int32_t unknown = -1;
    int32_t array_index = -1;
    int32_t path_index = -1;
    int16_t mode = 0;
    int16_t tail = 0;
  } changed;
  static_assert(sizeof(FullChange) == 64);
  using Notify = void (*)(void*, const FullChange&);
  reinterpret_cast<Notify>(notification)(entity, changed);
  return true;
}

}  // namespace modlock::gameinterop
