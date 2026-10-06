#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
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

// ISchemaSystem appends FindTypeScopeForModule after IAppSystem's eleven
// slots. ISchemaSystemTypeScope declares FindDeclaredClass third and
// FindDeclaredEnum fourth.
inline constexpr size_t kSchemaSystemFindTypeScopeSlot = 13;
inline constexpr size_t kTypeScopeFindDeclaredClassSlot = 2;
inline constexpr size_t kTypeScopeFindDeclaredEnumSlot = 3;

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

// SchemaField is one resolved schema field: its byte offset and bounded
// storage span inside the entity, plus whether the game networks it.
struct SchemaField {
  size_t offset;
  size_t size;
  bool networked;
};

// CanNotifyEntityStateChanged checks the current entity's replication entry before
// a batch of native writes. The entity is borrowed on the engine frame thread.
[[nodiscard]] MODLOCK_API bool CanNotifyEntityStateChanged(void* entity);

// NotifyEntityStateChanged publishes a full-entity network change after native
// schema writes. The entity is borrowed on the current engine frame thread.
[[nodiscard]] MODLOCK_API bool NotifyEntityStateChanged(void* entity);

// SchemaFieldOf resolves class_name.field_name at runtime through the game's
// schema system, walking base classes when the field
// belongs to an ancestor. schema_system is the live ISchemaSystem instance.
[[nodiscard]] MODLOCK_API std::expected<SchemaField, std::string> SchemaFieldOf(
    void* schema_system, const char* module_name, const char* class_name, const char* field_name);

// SchemaClassOfEntity returns the schema class info (SchemaClassInfoData_t)
// the engine binds to a live entity: CEntityIdentity::m_pClass,
// CEntityClass::m_pClassInfo and CEntityClassInfo::m_pSchemaBinding, at the
// offsets sourcesdk public/entity2 declares. Each link is copied through
// read, and the class info must point to itself as its own schema binding,
// so a layout a game update moved reports an error instead of faulting.
[[nodiscard]] MODLOCK_API std::expected<const void*, std::string> SchemaClassOfEntity(
    void* entity, const BoundedReader& read);

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
