#include "modlock/gameinterop/entity_abi.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "schema_layout.h"

namespace modlock::gameinterop {
namespace {

// An entity reaches its schema class through CEntityIdentity::m_pClass,
// CEntityClass::m_pClassInfo and CEntityClassInfo::m_pSchemaBinding
// (sourcesdk public/entity2/entityidentity.h and entityclass.h).
constexpr size_t kIdentityClass = 0x08;
constexpr size_t kEntityClassInfo = 0x58;
constexpr size_t kClassInfoSchemaBinding = 0x28;

// Teleport calls CBaseEntity::Teleport. Null pose pointers preserve placement.
// An unresolved slot leaves the entity in place.
void Teleport(void* entity, const float* position, const float* angles, const float* velocity) {
  // Find the entity's Teleport slot, resolved once: the resolver's answer,
  // a failure included, holds for the game build.
  if (entity == nullptr) {
    return;
  }
  static const auto slot = ResolveVirtualSlot("entity.teleport");
  if (!slot) {
    return;
  }

  // Call it through the entity's vtable.
  using TeleportFn = void (*)(void*, const float*, const float*, const float*);
  auto** table = *static_cast<void***>(entity);
  reinterpret_cast<TeleportFn>(table[*slot])(entity, position, angles, velocity);
}

// NetworkStateNotificationOf resolves the shared replication entry for preflight
// and publication, so callers never maintain a second copy of its ABI slot.
void* NetworkStateNotificationOf(void* entity) {
  // Find the entity's notification slot, resolved once since replication
  // asks on every state change.
  if (!entity) {
    return nullptr;
  }
  static const auto slot = ResolveVirtualSlot("entity.network-state-changed");
  if (!slot) {
    return nullptr;
  }

  // Read it from the entity's vtable.
  auto* table = *static_cast<void***>(entity);
  return table ? table[*slot] : nullptr;
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
  if (base == nullptr || !read(static_cast<const uint8_t*>(base) + offset, &value, sizeof(value))) {
    return nullptr;
  }
  return value;
}

// SchemaClassInfoOf borrows the declared class from the module's type scope.
std::expected<const schema::ClassInfo*, std::string> SchemaClassInfoOf(void* schema_system,
                                                                       const char* module_name,
                                                                       const char* class_name) {
  // Find the module's scope and the class declared in it.
  auto scope = FindTypeScopeOf(schema_system, module_name);
  if (!scope) {
    return std::unexpected(scope.error());
  }
  auto info = FindDeclaredClassOf(*scope, class_name);
  if (!info) {
    return std::unexpected(info.error());
  }
  return static_cast<const schema::ClassInfo*>(*info);
}

// IsFieldNetworked reports whether the field may be networked. Game build
// 6711 no longer ships MNetworkEnable metadata in the server schema, so a
// field counts as networked unless it carries MNetworkDisable; notifying an
// unnetworked field is harmless.
bool IsFieldNetworked(const schema::Field& field) {
  for (int32_t i = 0; i < field.metadata_count; ++i) {
    const char* name = field.metadata[i].name;
    if (name != nullptr && std::strcmp(name, "MNetworkDisable") == 0) {
      return false;
    }
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
// end of its class. type receives the field's schema type.
std::expected<SchemaField, std::string> FieldInClassInfo(const schema::ClassInfo* info,
                                                         const char* field_name,
                                                         const schema::Type** type) {
  for (; info != nullptr; info = FirstBaseOf(info)) {
    const std::span fields(info->fields, info->field_count);
    for (const auto& field : fields) {
      // Size the named field up to the next field's offset.
      if (field.name == nullptr || std::strcmp(field.name, field_name) != 0) {
        continue;
      }
      int32_t boundary = info->size;
      for (const auto& next : fields) {
        if (next.offset > field.offset && (boundary <= field.offset || next.offset < boundary)) {
          boundary = next.offset;
        }
      }
      if (field.offset < 0 || boundary <= field.offset) {
        return std::unexpected("schema field storage span is invalid");
      }
      *type = field.type;
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
  // Read the identity, which must point back at the entity.
  auto* identity = static_cast<const uint8_t*>(IdentityOf(entity));
  if (identity == nullptr || ReadPointer(identity, 0) != entity) {
    return std::nullopt;
  }

  // A deleted or deleting entity has no handle.
  uint32_t stored = 0;
  uint32_t flags = 0;
  std::memcpy(&stored, identity + kIdentityEHandleOffset, sizeof(stored));
  std::memcpy(&flags, identity + kIdentityFlagsOffset, sizeof(flags));
  return LiveHandleOf(stored, flags);
}

std::expected<const void*, std::string> SchemaClassOfEntity(void* entity,
                                                            const BoundedReader& read) {
  const void* identity = ReadLink(entity, kIdentityOffset, read);
  if (identity == nullptr || ReadLink(identity, 0, read) != entity) {
    return std::unexpected("the entity has no live identity");
  }
  return SchemaClassOfEntityClass(ReadLink(identity, kIdentityClass, read), read);
}

std::expected<const void*, std::string> SchemaClassOfEntityClass(const void* entity_class,
                                                                 const BoundedReader& read) {
  const void* binding =
      ReadLink(ReadLink(entity_class, kEntityClassInfo, read), kClassInfoSchemaBinding, read);
  if (binding == nullptr || ReadLink(binding, 0, read) != binding) {
    return std::unexpected("the entity's schema class is unreadable");
  }
  return binding;
}

std::string_view SchemaClassNameOf(const void* class_info) {
  const char* name = static_cast<const schema::ClassInfo*>(class_info)->name;
  return name == nullptr ? std::string_view() : std::string_view(name);
}

bool SchemaClassDerivesFrom(const void* class_info, std::string_view class_name) {
  for (auto* info = static_cast<const schema::ClassInfo*>(class_info); info != nullptr;
       info = FirstBaseOf(info)) {
    if (SchemaClassNameOf(info) == class_name) {
      return true;
    }
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
  // An unresolved system has no entities.
  std::vector<void*> instances;
  if (!entity_system) {
    return instances;
  }

  // CConcreteEntityList follows the vtable and current resource manifest.
  // Each of its 64 chunks contains 512 CEntityIdentity records of 0x70 bytes.
  const auto* chunks = static_cast<const uint8_t*>(entity_system) + 0x10;
  for (uint32_t chunk = 0; chunk < kMaxEntityLists; ++chunk) {
    const auto* entries = static_cast<const uint8_t*>(ReadPointer(chunks, chunk * sizeof(void*)));
    if (!entries) {
      continue;
    }
    for (uint32_t index = 0; index < 512; ++index) {
      // Each live entry whose handle matches its index is an instance.
      const auto* identity = entries + index * 0x70;
      void* instance = ReadPointer(identity, 0);
      if (!instance || IdentityOf(instance) != identity) {
        continue;
      }
      auto handle = ReferenceHandleOf(instance);
      if (handle && (*handle & 0x7FFF) == chunk * 512 + index) {
        instances.push_back(instance);
      }
    }
  }
  return instances;
}

void* EntityInstance(void* entity_system, uint32_t handle) {
  // A missing system or invalid handle names no entity.
  if (!entity_system || handle == 0xFFFFFFFFu) {
    return nullptr;
  }

  // Find the handle's chunk.
  const uint32_t index = handle & 0x7FFF;
  const auto* chunks = static_cast<const uint8_t*>(entity_system) + 0x10;
  const auto* chunk =
      static_cast<const uint8_t*>(ReadPointer(chunks, (index / 512) * sizeof(void*)));
  if (!chunk) {
    return nullptr;
  }

  // The identity must point at a live entity holding this handle.
  const auto* identity = chunk + (index % 512) * 0x70;
  void* instance = ReadPointer(identity, 0);
  if (!instance || IdentityOf(instance) != identity || ReferenceHandleOf(instance) != handle) {
    return nullptr;
  }
  return instance;
}

namespace {

// TraceResourceService prints the resource service's first slots when
// diagnosing a shifted native layout.
void TraceResourceService(void* resource_service) {
  // Trace only when interop tracing is on.
  static const bool trace = InteropTraceEnabled();
  if (!trace) {
    return;
  }

  // Print the service pointer and each slot.
  const auto* service = static_cast<const unsigned char*>(resource_service);
  std::fprintf(stderr, "[modlock] interop trace: resource_service=%p\n", resource_service);
  for (size_t offset = 0; offset < 0x100; offset += sizeof(void*)) {
    void* value = nullptr;
    std::memcpy(&value, service + offset, sizeof(value));
    std::fprintf(stderr, "[modlock] interop trace: service+0x%02zx = %p\n", offset, value);
  }
  std::fflush(stderr);
}

}  // namespace

std::expected<void*, std::string> ResolveLiveEntitySystem() {
  // Borrow the game resource service once. The engine keeps one for the
  // process, and finding it walks the loaded modules and the interface list,
  // which every pawn read would otherwise pay. A failure resolves again on
  // the next call, since the engine may not have loaded yet.
  static std::atomic<void*> cached{nullptr};
  void* resource_service = cached.load(std::memory_order_acquire);
  if (resource_service == nullptr) {
    // Find it through the engine's factory and keep it.
    const auto resolved = ResolveEngineInterface(L"engine2.dll", kGameResourceServiceServerVersion);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    resource_service = *resolved;
    cached.store(resource_service, std::memory_order_release);
    TraceResourceService(resource_service);
  }

  // The entity system changes with the map, so read it on every call.
  return EntitySystemFromResourceService(resource_service);
}

std::expected<void*, std::string> ResolveSchemaSystem() {
  return ResolveEngineInterface(L"schemasystem.dll", "SchemaSystem_001");
}

std::expected<void*, std::string> FindTypeScopeOf(void* schema_system, const char* module_name) {
  // Find the scope through the schema system's vtable.
  if (!schema_system) {
    return std::unexpected("the schema system instance is not resolved yet");
  }
  const auto slot = ResolveVirtualSlot("schema-system.find-type-scope");
  if (!slot) {
    return std::unexpected(slot.error());
  }
  using FindTypeScope = void* (*)(void*, const char*, const char**);
  auto* scope =
      VtableCall<FindTypeScope>(schema_system, *slot)(schema_system, module_name, nullptr);
  if (!scope) {
    return std::unexpected("schema type scope '" + std::string(module_name) + "' is not open");
  }
  return scope;
}

namespace {

// FindDeclared calls a type scope lookup with the hidden-return ABI described
// at FindDeclaredClassOf in the header.
std::expected<void*, std::string> FindDeclared(void* type_scope, std::string_view slot_id,
                                               const char* kind, const char* name) {
  // Find the declaration through the scope's vtable.
  if (type_scope == nullptr) {
    return std::unexpected("the schema type scope instance is not resolved yet");
  }
  const auto slot = ResolveVirtualSlot(slot_id);
  if (!slot) {
    return std::unexpected(slot.error());
  }
  using FindFn = void (*)(void* type_scope, void* out_handle, const char* name);
  void* info = nullptr;
  VtableCall<FindFn>(type_scope, *slot)(type_scope, &info, name);
  if (info == nullptr) {
    return std::unexpected(std::string("schema ") + kind + " '" + name + "' is not declared");
  }
  return info;
}

}  // namespace

std::expected<void*, std::string> FindDeclaredClassOf(void* type_scope, const char* class_name) {
  return FindDeclared(type_scope, "schema-type-scope.find-declared-class", "class", class_name);
}

std::expected<void*, std::string> FindDeclaredEnumOf(void* type_scope, const char* enum_name) {
  return FindDeclared(type_scope, "schema-type-scope.find-declared-enum", "enum", enum_name);
}

namespace {

// TemplateArgument returns the argument of a one-argument template type
// spelled as the schema spells it, such as CHandle< CBaseEntity >, or nothing
// when type is not that template.
std::optional<std::string_view> TemplateArgument(std::string_view type, std::string_view name) {
  if (!type.starts_with(name) || !type.substr(name.size()).starts_with("< ") ||
      !type.ends_with(" >")) {
    return std::nullopt;
  }
  return type.substr(name.size() + 2, type.size() - name.size() - 4);
}

// NamedStorageOf classifies a builtin or engine type by its name, or answers
// nothing for a name it does not know. A fixed array reads as its first
// element.
std::optional<SchemaStorage> NamedStorageOf(std::string_view type) {
  static constexpr std::array<std::string_view, 13> kNumbers{
      "bool",   "int8",   "int16",   "int32",   "int64",         "uint8",          "uint16",
      "uint32", "uint64", "float32", "float64", "CEntityHandle", "CUtlStringToken"};
  if (std::ranges::find(kNumbers, type) != kNumbers.end() || TemplateArgument(type, "CHandle")) {
    return SchemaStorage::kNumber;
  }
  if (type == "Vector" || type == "VectorWS" || type == "QAngle") {
    return SchemaStorage::kVector;
  }
  if (type == "CUtlString" || type == "CUtlSymbolLarge") {
    return SchemaStorage::kText;
  }
  if (type.ends_with(']')) {
    return NamedStorageOf(type.substr(0, type.rfind('[')));
  }
  return std::nullopt;
}

// IsBoxed reports whether a class is a boxed number, such as GameTime_t: one
// field the schema marks MIsBoxedFloatType or MIsBoxedIntegerType.
bool IsBoxed(const schema::ClassInfo& info) {
  if (info.field_count != 1) {
    return false;
  }
  for (int32_t i = 0; i < info.metadata_count; ++i) {
    const char* name = info.metadata[i].name;
    if (name != nullptr && std::string_view(name).starts_with("MIsBoxed")) {
      return true;
    }
  }
  return false;
}

// BoxedStorageOf classifies a declared class: a boxed number reads as its
// number, any other class is opaque.
SchemaStorage BoxedStorageOf(const schema::ClassInfo& info) {
  const auto* type = info.fields == nullptr ? nullptr : info.fields->type;
  if (!IsBoxed(info) || type == nullptr || type->name == nullptr) {
    return SchemaStorage::kOpaque;
  }
  return NamedStorageOf(type->name).value_or(SchemaStorage::kOpaque);
}

// DeclaredStorageOf classifies a type that declares an enum or a class from
// the declaration its record holds. A declaration that does not point back at
// itself under the type's name is a record of another shape, and opaque.
SchemaStorage DeclaredStorageOf(const schema::Type* type) {
  // Find the declaration of a declared class or enum.
  const auto* declared = reinterpret_cast<const schema::DeclaredType*>(type);
  const auto category = declared->category;
  if (category != schema::TypeCategory::kDeclaredClass &&
      category != schema::TypeCategory::kDeclaredEnum) {
    return SchemaStorage::kOpaque;
  }
  const auto* info = static_cast<const schema::ClassInfo*>(declared->declaration);
  if (info == nullptr || info->self != info || info->name == nullptr ||
      std::strcmp(info->name, type->name) != 0) {
    return SchemaStorage::kOpaque;
  }

  // An enum is a number; its record shares the class record's self and name.
  if (category == schema::TypeCategory::kDeclaredEnum) {
    return SchemaStorage::kNumber;
  }
  return BoxedStorageOf(*info);
}

// ElementStorageOf classifies a list's element type, which its list's record
// names but does not declare, by name and then in the module's type scope. A
// type the scope does not declare is opaque.
SchemaStorage ElementStorageOf(void* schema_system, const char* module_name,
                               std::string_view element) {
  if (auto named = NamedStorageOf(element)) {
    return *named;
  }
  if (element.empty() || element.find_first_of("<>*[]: ") != std::string_view::npos) {
    return SchemaStorage::kOpaque;
  }
  auto scope = FindTypeScopeOf(schema_system, module_name);
  if (!scope) {
    return SchemaStorage::kOpaque;
  }
  const std::string name(element);
  if (auto info = FindDeclaredClassOf(*scope, name.c_str())) {
    return BoxedStorageOf(*static_cast<const schema::ClassInfo*>(*info));
  }
  return FindDeclaredEnumOf(*scope, name.c_str()) ? SchemaStorage::kNumber : SchemaStorage::kOpaque;
}

// Classify fills field's storage from its schema type.
void Classify(void* schema_system, const char* module_name, const schema::Type* type,
              SchemaField& field) {
  if (type == nullptr || type->name == nullptr) {
    return;
  }
  field.type = type->name;
  for (const auto list : {"CUtlVector", "CNetworkUtlVectorBase"}) {
    if (auto element = TemplateArgument(type->name, list)) {
      field.list = true;
      field.storage = ElementStorageOf(schema_system, module_name, *element);
      return;
    }
  }
  auto named = NamedStorageOf(type->name);
  field.storage = named ? *named : DeclaredStorageOf(type);
}

}  // namespace

std::expected<SchemaField, std::string> SchemaFieldOf(void* schema_system, const char* module_name,
                                                      const char* class_name,
                                                      const char* field_name) {
  // Find the class that holds the path's first field.
  auto info = SchemaClassInfoOf(schema_system, module_name, class_name);
  if (!info) {
    return std::unexpected(info.error());
  }

  // A dotted path walks embedded structures: each segment is a field of the
  // class the previous segment holds, and the offsets add up. The last
  // segment's storage stays inside its own structure.
  std::string_view path = field_name;
  size_t base = 0;
  bool networked = true;
  for (;;) {
    // Find the segment's field; the last one ends the walk.
    const auto dot = path.find('.');
    const std::string segment(path.substr(0, dot));
    const schema::Type* type = nullptr;
    auto field = FieldInClassInfo(*info, segment.c_str(), &type);
    if (!field) {
      return std::unexpected(field.error());
    }
    networked = networked && field->networked;
    if (dot == std::string_view::npos) {
      SchemaField found{base + field->offset, field->size, networked};
      Classify(schema_system, module_name, type, found);
      return found;
    }

    // Step into the structure the segment holds.
    if (type == nullptr || type->name == nullptr) {
      return std::unexpected("schema field '" + segment + "' has no type");
    }
    info = SchemaClassInfoOf(schema_system, module_name, type->name);
    if (!info) {
      return std::unexpected("schema field '" + segment +
                             "' does not hold a structure: " + info.error());
    }
    base += field->offset;
    path.remove_prefix(dot + 1);
  }
}

std::expected<void, std::string> ResolveSchemaOffsets(void* schema_system, const char* module_name,
                                                      std::span<const SchemaOffset> fields) {
  for (const auto& wanted : fields) {
    // Find the field; a missing or short one names itself in the error.
    const auto name = std::string(wanted.class_name) + "." + wanted.field_name;
    auto field = SchemaFieldOf(schema_system, module_name, wanted.class_name, wanted.field_name);
    if (!field) {
      return std::unexpected(name + ": " + field.error());
    }
    if (field->size < wanted.minimum_size) {
      return std::unexpected(name + " storage span is too small");
    }
    *wanted.offset = field->offset;
  }
  return {};
}

size_t ResolveOptionalSchemaOffsets(void* schema_system, const char* module_name,
                                    std::span<const OptionalSchemaOffset> fields) {
  size_t filled = 0;
  for (const auto& wanted : fields) {
    auto field = SchemaFieldOf(schema_system, module_name, wanted.class_name, wanted.field_name);
    if (field && field->size >= wanted.minimum_size) {
      *wanted.offset = field->offset;
      ++filled;
    }
  }
  return filled;
}

std::expected<size_t, std::string> SchemaClassSizeOf(void* schema_system, const char* module_name,
                                                     const char* class_name) {
  auto info = SchemaClassInfoOf(schema_system, module_name, class_name);
  if (!info) {
    return std::unexpected(info.error());
  }
  if ((*info)->size <= 0) {
    return std::unexpected("schema class allocation size is invalid");
  }
  return static_cast<size_t>((*info)->size);
}

std::expected<int64_t, std::string> SchemaEnumValueOf(void* schema_system, const char* module_name,
                                                      const char* enum_name,
                                                      const char* value_name) {
  // Find the enum in the module's scope.
  auto scope = FindTypeScopeOf(schema_system, module_name);
  if (!scope) {
    return std::unexpected(scope.error());
  }
  auto found = FindDeclaredEnumOf(*scope, enum_name);
  if (!found) {
    return std::unexpected(found.error());
  }

  // Return the named value.
  const auto* info = static_cast<const schema::EnumInfo*>(*found);
  for (const auto& enumerator : std::span(info->enumerators, info->enumerator_count)) {
    if (std::strcmp(enumerator.name, value_name) == 0) {
      return enumerator.value;
    }
  }
  return std::unexpected("schema enum value '" + std::string(enum_name) + "." + value_name +
                         "' is not declared");
}

bool CanNotifyEntityStateChanged(void* entity) {
  return NetworkStateNotificationOf(entity) != nullptr;
}

bool NotifyEntityStateChanged(void* entity) {
  // Find the entity's notification call.
  auto* notification = NetworkStateNotificationOf(entity);
  if (!notification) {
    return false;
  }

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

  // Send a full change.
  static_assert(sizeof(FullChange) == 64);
  using Notify = void (*)(void*, const FullChange&);
  reinterpret_cast<Notify>(notification)(entity, changed);
  return true;
}

}  // namespace modlock::gameinterop
