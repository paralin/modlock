#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/keyvalues.h"
#include "modlock/gameinterop/native_memory.h"

namespace modlock::gameinterop {

// kIdentityOffset is CEntityInstance::m_pEntity (CEntityIdentity*), read for
// QueueSpawnEntity's identity argument.
//
// Source SDK public/entity2/entityinstance.h places this member at 0x10.
inline constexpr size_t kIdentityOffset = 0x10;

// kResourceServiceEntitySystemOffset is CGameResourceService::m_pEntitySystem.
// The resource service arrives from engine2.dll's CreateInterface under
// "GameResourceServiceServerV001".
//
// The live client build proves the offset at 0x58, not the sourcesdk mirror's
// 0x38: decoding the service's own vtable shows the CBaseEngineService
// accessors (GetName/IsActive/SetActive/Get|SetServiceIndex) operate on
// 0x28/0x30/0x32, a +0x20 shift against the mirror. That shift moves every
// later member the same step, and 0x58 is the only heap-pointer-shaped field
// in the first 0x100 bytes. MODLOCK_INTEROP_TRACE=1 records the full window
// each process.
inline constexpr size_t kResourceServiceEntitySystemOffset = 0x58;

// Engine2.dll's IGameResourceService interface version (sourcesdk
// public/interfaces/interfaces.h).
inline constexpr char kGameResourceServiceServerVersion[] = "GameResourceServiceServerV001";

// Layout constants follow the Source SDK entity declarations:
// public/entity2/entityidentity.h (CEntityIdentity "Size: 0x70", field
// comments), public/entityhandle.h (m_Parts bitfields), and
// entity2/entitysystem.h plus entity2/concreteentitylist.h (CGameEntitySystem
// member order and the chunk table).
inline constexpr uint32_t kMaxEntitiesInList = 512;          // MAX_ENTITIES_IN_LIST
inline constexpr uint32_t kMaxEntityLists = 64;              // MAX_ENTITY_LISTS
inline constexpr size_t kIdentitySize = 0x70;                // sizeof(CEntityIdentity)
inline constexpr size_t kIdentityInstanceOffset = 0x00;      // m_pInstance
inline constexpr size_t kIdentityEHandleOffset = 0x10;       // m_EHandle
inline constexpr size_t kIdentityDesignerNameOffset = 0x20;  // m_designerName
inline constexpr size_t kIdentityFlagsOffset = 0x30;         // m_flags
inline constexpr uint32_t kEntityIndexBits = 0x7FFF;         // m_Parts.m_EntityIndex : 15
inline constexpr uint32_t kSerialShift = 15;                 // m_Parts.m_Serial : 17
inline constexpr uint32_t kSerialBits = 0x1FFFF;
inline constexpr uint32_t kInvalidEHandleIndex = 0xFFFFFFFF;
inline constexpr uint32_t kEfInvalidEHandle = 0x1;  // EF_IS_INVALID_EHANDLE
inline constexpr uint32_t kEfDeleteFlags =
    0x10 | 0x200;  // EF_DELETE_IN_PROGRESS | EF_MARKED_FOR_DELETE

// Offset of CConcreteEntityList m_EntityList inside CGameEntitySystem: the
// vptr and IEntityResourceManifest* m_pCurrentManifest precede it.
inline constexpr size_t kEntityListOffset = 0x10;

// EntityIndexOf returns the entity index a handle names, its low 15 bits.
constexpr uint32_t EntityIndexOf(uint32_t handle) {
  return handle & kEntityIndexBits;
}

// LiveHandleOf returns the handle an identity's stored handle and flags
// prove, as CEntityIdentity::GetRefEHandle computes it: the stored index with
// the serial corrected by EF_IS_INVALID_EHANDLE. An entity being deleted has
// none.
constexpr std::optional<uint32_t> LiveHandleOf(uint32_t stored, uint32_t flags) {
  if (stored == kInvalidEHandleIndex || (flags & kEfDeleteFlags) != 0) {
    return std::nullopt;
  }
  const uint32_t serial = ((stored >> kSerialShift) - (flags & kEfInvalidEHandle)) & kSerialBits;
  return EntityIndexOf(stored) | (serial << kSerialShift);
}

// FindTypeScopeOf returns the type scope of a loaded module, such as
// "server.dll", or an error when the module declares no schema.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> FindTypeScopeOf(
    void* schema_system, const char* module_name);

// FindDeclaredClassOf invokes the type scope's FindDeclaredClass with the
// MSVC x64 hidden-return ABI the live binary proves: the returned handle
// carries user-provided constructors (sourcesdk public/schemasystem/
// schematypes.h SchemaMetaInfoHandle_t), so the vtable entry consumes
// (type_scope, SchemaMetaInfoHandle_t* out, const char* class_name) and the
// class info lands in the caller's out slot, never in RAX.
//
// The host9 dump pinned this: schemasystem.dll's null path writes through its
// RDX argument, which a name-in-RDX call aims at read-only .rdata.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> FindDeclaredClassOf(
    void* type_scope, const char* class_name);

// FindDeclaredEnumOf returns a type scope's enum info with the same ABI.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> FindDeclaredEnumOf(
    void* type_scope, const char* enum_name);

// InteropTraceEnabled reports whether MODLOCK_INTEROP_TRACE=1 asks native
// interop to log its walk stages. It reads the environment on every call, so
// per-frame callers cache the result.
[[nodiscard]] MODLOCK_API bool InteropTraceEnabled();

// TeleportEntity applies position, Euler angles in degrees, and velocity through
// the engine's CBaseEntity::Teleport slot 163. A zero velocity stops motion. A
// null entity is ignored.
MODLOCK_API void TeleportEntity(void* entity, const std::array<float, 3>& position,
                                const std::array<float, 3>& angles,
                                const std::array<float, 3>& velocity);
// The coordinate overload resets angles and velocity to zero.
MODLOCK_API void TeleportEntity(void* entity, float x, float y, float z);
// The pointer overload leaves each null part unchanged.
MODLOCK_API void TeleportEntity(void* entity, const float* position, const float* angles,
                                const float* velocity);
// SetEntityVelocity changes native velocity without moving or turning the entity.
MODLOCK_API void SetEntityVelocity(void* entity, const std::array<float, 3>& velocity);

// VariantType is the datamap field type a Variant holds, numbered as the
// game's fieldtype_t enum.
enum class VariantType : std::uint8_t {
  kVoid = 0,
  kFloat32 = 1,
  kVector = 3,
  kInt32 = 5,
  kBoolean = 6,
  kColor32 = 9,
  kCString = 30,
};

// Variant is Source 2's variant_t, the value AcceptInput passes to an input:
// an eight-byte value, an 8-bit field type, one byte of alignment, and 16-bit
// flags. An int32, float32, bool or color sits in the value; a string or
// vector is a pointer to storage the caller keeps. Zero flags tell the game it
// does not own that storage, and borrowing is safe because AcceptInput
// consumes the variant synchronously. Inputs read the value as its type says
// and do not convert text.
struct alignas(8) Variant {
  union {
    const void* pointer = nullptr;
    std::int32_t int32;
    float float32;
    bool boolean;
    KeyValueColor color;
  };

  VariantType type = VariantType::kVoid;
  std::uint8_t alignment = 0;
  std::uint16_t flags = 0;
};

static_assert(sizeof(Variant) == 16);

// IdentityOf reads the entity-identity pointer QueueSpawnEntity consumes.
[[nodiscard]] MODLOCK_API void* IdentityOf(void* entity);

// ReferenceHandleOf validates entity's identity back-pointer and returns the
// serial-corrected packed handle the entity system uses, or no value when the
// entity is null, detached, or marked with an invalid handle.
[[nodiscard]] MODLOCK_API std::optional<uint32_t> ReferenceHandleOf(void* entity);

// EntityInstances copies current live entity pointers from the native chunk list.
// Borrowed instances are valid only until the next engine mutation or frame.
[[nodiscard]] MODLOCK_API std::vector<void*> EntityInstances(void* entity_system);
// EntityInstance resolves a serial-fenced handle in the current entity list.
[[nodiscard]] MODLOCK_API void* EntityInstance(void* entity_system, uint32_t handle);

// EntityRef names one entity an owner created by its serial-fenced handle and
// resolves the instance on each use. The engine may remove the entity first,
// as it removes a prop it rejects or everything on a map change; the reference
// then resolves to null, never to freed memory, so its owner can still clean
// up safely.
class EntityRef {
 public:
  EntityRef() = default;

  // EntityRef refers to entity, created in entity_system. An entity without a
  // live handle yields an empty reference.
  EntityRef(void* entity_system, void* entity)
      : system_(entity_system), handle_(ReferenceHandleOf(entity).value_or(kInvalidEHandleIndex)) {}

  // Get returns the live instance, or null once the entity is gone.
  [[nodiscard]] void* Get() const { return EntityInstance(system_, handle_); }

  // Handle returns the handle while the entity is live.
  [[nodiscard]] std::optional<uint32_t> Handle() const {
    return Get() ? std::optional(handle_) : std::nullopt;
  }

  // Reset forgets the entity without touching the engine.
  void Reset() { handle_ = kInvalidEHandleIndex; }

 private:
  void* system_ = nullptr;
  uint32_t handle_ = kInvalidEHandleIndex;
};

// EntitySystemFromResourceService reads CGameEntitySystem out of one live
// IGameResourceService instance. A null service is an error; nothing here
// guesses an address.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> EntitySystemFromResourceService(
    void* resource_service);

// ResolveLiveEntitySystem resolves the mapped engine2.dll resource service
// through its CreateInterface export and returns the global CGameEntitySystem.
// Windows host builds only; other platforms report the missing host.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> ResolveLiveEntitySystem();

// ResolveSchemaSystem resolves the mapped schemasystem.dll ISchemaSystem
// instance through its CreateInterface export ("SchemaSystem_001"), the same
// interface the pawn observer and the world-text factory resolve. Windows
// host builds only; other platforms report the missing host.
//
// Every failure is a named error; nothing here guesses an address.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> ResolveSchemaSystem();

// SchemaStorage is what a schema field's bytes hold, as far as a caller that
// reads or writes them by value may treat them.
enum class SchemaStorage : uint8_t {
  // kOpaque is anything else: a pointer, an embedded structure, a container
  // other than a list, or a type the schema does not describe. Its bytes are
  // not a value a caller may read or write.
  kOpaque,
  // kNumber is a boolean, an integer, a float, an enum, an entity handle or a
  // boxed number.
  kNumber,
  // kVector is three floats: a Vector, VectorWS or QAngle.
  kVector,
  // kText is a pointer to characters the game owns: a CUtlString or
  // CUtlSymbolLarge.
  kText,
};

// SchemaField is one resolved schema field: its byte offset and bounded
// storage span inside the entity, whether the game networks it, and what its
// storage holds. A list field is a CUtlVector whose elements hold storage.
// type is the schema's name for the field's type, static data of the
// declaring module.
struct SchemaField {
  size_t offset;
  size_t size;
  bool networked;
  SchemaStorage storage = SchemaStorage::kOpaque;
  bool list = false;
  const char* type = nullptr;
};

// CanNotifyEntityStateChanged checks the current entity's replication entry before
// a batch of native writes. The entity is borrowed on the engine frame thread.
[[nodiscard]] MODLOCK_API bool CanNotifyEntityStateChanged(void* entity);

// NotifyEntityStateChanged publishes a full-entity network change after native
// schema writes. The entity is borrowed on the current engine frame thread.
[[nodiscard]] MODLOCK_API bool NotifyEntityStateChanged(void* entity);

// SchemaFieldOf resolves class_name.field_name at runtime through the game's
// schema system, walking base classes when the field belongs to an ancestor.
// A dotted field_name, such as m_Collision.m_CollisionGroup, reaches a field
// inside an embedded structure, with its offset from the entity's start.
// The field's storage comes from its schema type, and a type it cannot
// classify is opaque rather than an error.
// schema_system is the live ISchemaSystem instance.
[[nodiscard]] MODLOCK_API std::expected<SchemaField, std::string> SchemaFieldOf(
    void* schema_system, const char* module_name, const char* class_name, const char* field_name);

// SchemaOffset names a schema field, the storage it must span, and the offset
// it fills.
struct SchemaOffset {
  const char* class_name;
  const char* field_name;
  size_t minimum_size;
  size_t* offset;
};

// ResolveSchemaOffsets fills each field's offset from the module's schema and
// stops at the first field that is missing or spans less than its minimum,
// returning an error that names it.
[[nodiscard]] MODLOCK_API std::expected<void, std::string> ResolveSchemaOffsets(
    void* schema_system, const char* module_name, std::span<const SchemaOffset> fields);

// OptionalSchemaOffset names a schema field the caller can do without, the
// storage it must span, and the offset it fills when the field is usable.
struct OptionalSchemaOffset {
  const char* class_name;
  const char* field_name;
  size_t minimum_size;
  std::optional<size_t>* offset;
};

// ResolveOptionalSchemaOffsets fills the offset of each field the module's
// schema has with at least its minimum span, leaves the others untouched, and
// returns how many it filled.
MODLOCK_API size_t ResolveOptionalSchemaOffsets(void* schema_system, const char* module_name,
                                                std::span<const OptionalSchemaOffset> fields);

// SchemaClassOfEntity returns the schema class info (SchemaClassInfoData_t)
// the engine binds to a live entity: CEntityIdentity::m_pClass,
// CEntityClass::m_pClassInfo and CEntityClassInfo::m_pSchemaBinding, at the
// offsets sourcesdk public/entity2 declares. Each link is copied through
// read, and the class info must point to itself as its own schema binding,
// so a layout a game update moved reports an error instead of faulting.
[[nodiscard]] MODLOCK_API std::expected<const void*, std::string> SchemaClassOfEntity(
    void* entity, const BoundedReader& read);

// SchemaClassOfEntityClass returns the schema class info a CEntityClass, the
// entity system's record of one designer name, binds. It reads the links
// after the identity's as SchemaClassOfEntity does.
[[nodiscard]] MODLOCK_API std::expected<const void*, std::string> SchemaClassOfEntityClass(
    const void* entity_class, const BoundedReader& read);

// SchemaClassNameOf returns the name of a schema class info, such as
// CCitadelPlayerPawn.
[[nodiscard]] MODLOCK_API std::string_view SchemaClassNameOf(const void* class_info);

// SchemaClassDerivesFrom reports whether class_info is the class named
// class_name or derives from it through first bases, the chain whose fields
// share the derived class's offsets.
[[nodiscard]] MODLOCK_API bool SchemaClassDerivesFrom(const void* class_info,
                                                      std::string_view class_name);

// SchemaClassSizeOf returns the native allocation size declared by this module.
[[nodiscard]] MODLOCK_API std::expected<size_t, std::string> SchemaClassSizeOf(
    void* schema_system, const char* module_name, const char* class_name);

// SchemaEnumValueOf resolves a named enumerator from the loaded module's schema.
// Callers retain the value only for that engine world's lifetime.
[[nodiscard]] MODLOCK_API std::expected<int64_t, std::string> SchemaEnumValueOf(
    void* schema_system, const char* module_name, const char* enum_name, const char* value_name);

}  // namespace modlock::gameinterop
