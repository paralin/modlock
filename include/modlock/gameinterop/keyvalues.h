#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// MemberName mirrors CKV3MemberName (tier1/keyvalues3.h): the 32-bit
// case-insensitive string-token hash, the always-invalid large-symbol id,
// and the original string pointer. MSVC x64 passes the 16-byte aggregate by
// reference to a caller-built temporary, which is the shape the scanned
// setters read.
struct MemberName {
  std::uint32_t hash;
  std::uint32_t symbol;
  const char* string;
};

// kMemberNameInvalidSymbol is UTL_INVAL_SYMBOL_LARGE from tier1/
// utlsymbollarge.h.
inline constexpr std::uint32_t kMemberNameInvalidSymbol = 0xFFFFFFFFu;

// MakeMemberName transcribes CKV3MemberName::Make: MurmurHash2 over the
// lowercased key with the recorded string-token seed (0x31415926), the
// invalid symbol id, and the key pointer itself.
//
// The result is the 16-byte aggregate the scanned setters read.
[[nodiscard]] MODLOCK_API MemberName MakeMemberName(std::string_view key);

// KeyValuesCalls is the resolved raw-ABI surface of CEntityKeyValues
// construction and its typed setters. Every slot is either a scanned game
// address or null; null slots fail their build step with a named error.
struct MODLOCK_API KeyValuesCalls {
  // CEntityKeyValues constructor shape: MemAlloc_Alloc(sizeof(
  // CEntityKeyValues)) followed by placement-new with the default
  // NORMAL-allocator arguments.
  void* (*create_key_values)() = nullptr;
  // Current engine-native construction surface. The allocator and constructor
  // are separate in server.dll; construct_key_values receives
  // (storage, nullptr, EKV_ALLOCATOR_NORMAL).
  void* (*allocate)(std::size_t size) = nullptr;
  void* (*construct_key_values)(void* storage, void* allocator,
                                unsigned char allocator_type) = nullptr;
  // CEntityKeyValues::SetKeyValue(id, false), returning the newly allocated
  // KeyValues3 member. BuildEntityKeyValues writes only fresh members.
  void* (*set_key_value)(void* ekv, const MemberName* name, unsigned char as_attribute) = nullptr;
  // High-level setter slots remain as an injection seam for contract tests.
  // Production uses the source-matched native surface above.
  // ekv->SetString(CKV3MemberName::Make(key), value); return ignored.
  void (*set_string)(void* ekv, const MemberName* name, const char* value) = nullptr;
  // ekv->SetBool(CKV3MemberName::Make(key), value).
  void (*set_bool)(void* ekv, const MemberName* name, unsigned char value) = nullptr;
  // ekv->SetInt(CKV3MemberName::Make(key), value).
  void (*set_int)(void* ekv, const MemberName* name, int value) = nullptr;
  // ekv->SetFloat(CKV3MemberName::Make(key), value).
  void (*set_float)(void* ekv, const MemberName* name, float value) = nullptr;
  // ekv->SetColor(CKV3MemberName::Make(key), Color(r, g, b, a)); the packed
  // word lays out as r | g << 8 | b << 16 | a << 24, matching Color's
  // little-endian byte order.
  void (*set_color)(void* ekv, const MemberName* name, std::uint32_t packed_rgba) = nullptr;
  // ekv->SetVector(CKV3MemberName::Make(key), Vector(x, y, z)); the triple
  // rides a caller-built temporary per the >8-byte aggregate rule.
  void (*set_vector)(void* ekv, const MemberName* name, const float* xyz) = nullptr;
};

// ResolveKeyValuesCalls resolves the recorded allocation, construction, and
// member-insertion signatures against image, or returns an error naming the
// first unresolved one.
[[nodiscard]] MODLOCK_API std::expected<KeyValuesCalls, std::string> ResolveKeyValuesCalls(
    const ModuleImage& image);

// AllocateEmptyEntityKeyValues returns a zeroed temporary representation accepted by
// QueueSpawnEntity when no key/value pairs are needed.
[[nodiscard]] MODLOCK_API void* AllocateEmptyEntityKeyValues();

// FreeEmptyEntityKeyValues releases storage allocated by AllocateEmptyEntityKeyValues.
MODLOCK_API void FreeEmptyEntityKeyValues(void* key_values);

// KeyValueColor is one RGBA color value.
struct KeyValueColor {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
  std::uint8_t alpha;
};

// KeyValueVector is one three-component vector value.
struct KeyValueVector {
  float x;
  float y;
  float z;
};

// EntityValue is a value a map gives an entity: a spawn key value or an
// input's parameter. The alternatives cover the kinds native entity
// construction and inputs read.
using EntityValue = std::variant<bool, int, float, std::string_view, KeyValueColor, KeyValueVector>;

// EntityKeyValue is one typed key/value pair for BuildEntityKeyValues.
struct EntityKeyValue {
  std::string_view key;
  EntityValue value;
};

// BuildEntityKeyValues creates a real CEntityKeyValues through calls and
// applies every pair through its matching setter. A missing constructor or
// setter fails with a named error before any partial state escapes.
//
// Strings and vectors use engine-external storage. Callers retain pairs and
// NUL-terminated string storage until ExecuteQueuedCreation consumes the result.
[[nodiscard]] MODLOCK_API std::expected<void*, std::string> BuildEntityKeyValues(
    const KeyValuesCalls& calls, std::span<const EntityKeyValue> pairs);

}  // namespace modlock::gameinterop
