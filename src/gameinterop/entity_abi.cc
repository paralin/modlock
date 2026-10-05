#include "modlock/gameinterop/entity_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "modlock/gameinterop/mapped_module_image.h"

namespace modlock::gameinterop {
namespace {

// Class-info field offsets follow sourcesdk public/schemasystem/schematypes.h
// (SchemaClassInfoData_t, SchemaClassFieldData_t, SchemaMetadataEntryData_t).
// Game build 6711 added m_pszCPPName after m_pszProjectName.
constexpr size_t kClassInfoSize = 0x20;
constexpr size_t kClassInfoFieldCount = 0x24;
constexpr size_t kClassInfoBaseClassCount = 0x29;
constexpr size_t kClassInfoFields = 0x30;
constexpr size_t kClassInfoBaseClasses = 0x38;
constexpr size_t kFieldName = 0x0;
constexpr size_t kFieldOffset = 0x10;
constexpr size_t kFieldMetadataCount = 0x14;
constexpr size_t kFieldMetadata = 0x18;
constexpr size_t kFieldStride = 32;
constexpr size_t kMetadataStride = 16;

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

// SchemaClassInfoOf borrows the declared class from the module's type scope.
std::expected<void*, std::string> SchemaClassInfoOf(void* schema_system, const char* module_name,
                                                    const char* class_name) {
  if (!schema_system) return std::unexpected("the schema system instance is not resolved yet");
  using FindTypeScope = void* (*)(void*, const char*, const char**);
  auto* scope = VtableCall<FindTypeScope>(schema_system, kSchemaSystemFindTypeScopeSlot)(
      schema_system, module_name, nullptr);
  if (!scope)
    return std::unexpected("schema type scope '" + std::string(module_name) + "' is not open");
  return FindDeclaredClassOf(scope, class_name);
}

// IsFieldNetworked reports whether the field may be networked. Game build
// 6711 no longer ships MNetworkEnable metadata in the server schema, so a
// field counts as networked unless it carries MNetworkDisable; notifying an
// unnetworked field is harmless.
bool IsFieldNetworked(const uint8_t* field) {
  int count = 0;
  std::memcpy(&count, field + kFieldMetadataCount, sizeof(count));
  auto* metadata = static_cast<const uint8_t*>(ReadPointer(field, kFieldMetadata));
  for (int i = 0; i < count; ++i) {
    const char* name = static_cast<const char*>(ReadPointer(metadata + i * kMetadataStride, 0));
    if (name != nullptr && std::strcmp(name, "MNetworkDisable") == 0) {
      return false;
    }
  }
  return true;
}

// FieldInClassInfo searches one class info's own fields, then its first base
// class (single inheritance).
std::expected<SchemaField, std::string> FieldInClassInfo(const void* class_info,
                                                         const char* field_name) {
  if (class_info == nullptr) {
    return std::unexpected("schema class is not declared in this module");
  }
  uint16_t field_count = 0;
  std::memcpy(&field_count, static_cast<const uint8_t*>(class_info) + kClassInfoFieldCount,
              sizeof(field_count));
  auto* fields = static_cast<const uint8_t*>(ReadPointer(class_info, kClassInfoFields));
  for (uint16_t i = 0; i < field_count; ++i) {
    const uint8_t* field = fields + i * kFieldStride;
    const char* name = static_cast<const char*>(ReadPointer(field, kFieldName));
    if (name != nullptr && std::strcmp(name, field_name) == 0) {
      int offset = 0;
      int class_size = 0;
      std::memcpy(&offset, field + kFieldOffset, sizeof(offset));
      std::memcpy(&class_size, static_cast<const uint8_t*>(class_info) + kClassInfoSize,
                  sizeof(class_size));
      int boundary = class_size;
      for (uint16_t next = 0; next < field_count; ++next) {
        int next_offset = 0;
        std::memcpy(&next_offset, fields + next * kFieldStride + kFieldOffset, sizeof(next_offset));
        if (next_offset > offset && (boundary <= offset || next_offset < boundary)) {
          boundary = next_offset;
        }
      }
      if (offset < 0 || boundary <= offset) {
        return std::unexpected("schema field storage span is invalid");
      }
      return SchemaField{static_cast<size_t>(offset), static_cast<size_t>(boundary - offset),
                         IsFieldNetworked(field)};
    }
  }
  uint8_t base_count = 0;
  std::memcpy(&base_count, static_cast<const uint8_t*>(class_info) + kClassInfoBaseClassCount,
              sizeof(base_count));
  if (base_count > 0) {
    // SchemaBaseClasses[i] = { unsigned offset; CSchemaClassInfo* class; }.
    auto* bases = static_cast<const uint8_t*>(ReadPointer(class_info, kClassInfoBaseClasses));
    return FieldInClassInfo(ReadPointer(bases, sizeof(void*) /* skip offset */), field_name);
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

std::expected<void*, std::string> FindDeclaredClassOf(void* type_scope, const char* class_name) {
  if (type_scope == nullptr) {
    return std::unexpected("the schema type scope instance is not resolved yet");
  }
  // Hidden-return ABI: see FindDeclaredClassOf in the header.
  using FindDeclaredClassFn = void (*)(void* type_scope, void* out_handle, const char* class_name);
  void* class_info = nullptr;
  VtableCall<FindDeclaredClassFn>(type_scope, kTypeScopeFindDeclaredClassSlot)(
      type_scope, &class_info, class_name);
  if (class_info == nullptr) {
    return std::unexpected("schema class '" + std::string(class_name) + "' is not declared");
  }
  return class_info;
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
  int32_t size = 0;
  std::memcpy(&size, static_cast<const uint8_t*>(*info) + kClassInfoSize, sizeof(size));
  if (size <= 0) return std::unexpected("schema class allocation size is invalid");
  return static_cast<size_t>(size);
}

std::expected<int64_t, std::string> SchemaEnumValueOf(void* schema_system, const char* module_name,
                                                      const char* enum_name,
                                                      const char* value_name) {
  if (!schema_system) return std::unexpected("the schema system instance is not resolved yet");
  using FindTypeScope = void* (*)(void*, const char*, const char**);
  auto* scope = VtableCall<FindTypeScope>(schema_system, kSchemaSystemFindTypeScopeSlot)(
      schema_system, module_name, nullptr);
  if (!scope)
    return std::unexpected("schema type scope '" + std::string(module_name) + "' is not open");
  // FindDeclaredEnum follows FindDeclaredClass and uses the same hidden return.
  using FindEnum = void (*)(void*, void*, const char*);
  const uint8_t* info = nullptr;
  VtableCall<FindEnum>(scope, 3)(scope, &info, enum_name);
  if (!info) return std::unexpected("schema enum '" + std::string(enum_name) + "' is not declared");
  // SchemaEnumInfoData_t: count +28, enumerators +32. Each enumerator is
  // {name, int64 value, metadata count, metadata pointer}, with stride 32.
  uint16_t count = 0;
  std::memcpy(&count, info + 28, sizeof(count));
  const auto* entries = static_cast<const uint8_t*>(ReadPointer(info, 32));
  for (uint16_t i = 0; i < count; ++i) {
    const auto* entry = entries + static_cast<size_t>(i) * 32;
    const auto* name = static_cast<const char*>(ReadPointer(entry, 0));
    if (std::strcmp(name, value_name) != 0) continue;
    int64_t value = 0;
    std::memcpy(&value, entry + 8, sizeof(value));
    return value;
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
