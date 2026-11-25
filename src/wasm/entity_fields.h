#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "modlock/gameinterop/entity_abi.h"
#include "src/wasm/fields/fields.pb.h"

// Entity field access reads and writes a live entity's schema fields, as the
// host's field calls and the world's spawn preparation do. Each function finds
// offsets through GameServices and touches memory only through the operating
// system's checked copies, so a field the entity lacks fails instead of
// faulting the server. Everything runs on the engine thread.

namespace modlock::wasm {

class GameServices;

// Size returns the bytes a field type occupies in the entity.
size_t Size(fields::FieldType type);

// Decode converts the bytes of one field to its value.
std::expected<fields::FieldValue, std::string> Decode(fields::FieldType type,
                                                      const unsigned char* bytes);

// Encode converts a value to the bytes of one field of type. The value must
// take the form the type reads as. Text is not encoded here, since its field
// points to the characters; StoreField writes a symbol field itself.
std::expected<std::array<unsigned char, 12>, std::string> Encode(fields::FieldType type,
                                                                 const fields::FieldValue& value);

// ReadValue copies the field of type at address through the operating system,
// so a field the object lacks fails instead of faulting the server, and
// decodes it. Name names the field in an error.
std::expected<fields::FieldResponse, std::string> ReadValue(const unsigned char* address,
                                                            fields::FieldType type,
                                                            const std::string& name);

// Holds checks that field's storage holds a value of type, or a list of them
// when list is set, as the schema declares it, so a mod reads and writes only
// the values the game keeps there and never a pointer or a structure. A
// refusal names the field as class_name.field_name.
std::expected<void, std::string> Holds(const gameinterop::SchemaField& field,
                                       fields::FieldType type, bool list,
                                       const std::string& class_name,
                                       const std::string& field_name);

// FieldAddress resolves a schema field of entity whose storage holds a
// value of type, or a list of them when list is set, refusing an entity
// that is not of class_name. A game rules class resolves on the game rules
// proxy entity to the field of the rules object, a model part class
// through the entity's body component, and a movement services class
// through the pawn's movement services.
std::expected<unsigned char*, std::string> FieldAddress(GameServices& services, void* entity,
                                                        const std::string& class_name,
                                                        const std::string& field,
                                                        fields::FieldType type, bool list);

// StoreField writes a schema field of entity without telling the network
// it changed. Text goes to a symbol field with addon,
// the mod's content addon, in place of {addon}.
std::expected<void, std::string> StoreField(GameServices& services, void* entity,
                                            const std::string& class_name, const std::string& field,
                                            fields::FieldType type, const fields::FieldValue& value,
                                            std::string_view addon);

// ReadEntityField reads the field request names on its live entity.
std::expected<fields::FieldResponse, std::string> ReadEntityField(
    GameServices& services, const fields::ReadFieldRequest& request);

// SceneNode returns entity's scene node, or null for an entity with no place
// in the world. It trusts entity to be live, as every caller's comes from
// the entity list this frame.
unsigned char* SceneNode(GameServices& services, void* entity);

// SceneVector reads a three-float field of entity's scene node, such as
// m_vecAbsOrigin or m_angAbsRotation.
std::optional<std::array<float, 3>> SceneVector(GameServices& services, void* entity,
                                                const std::string& field);

// Origin returns where entity is.
std::optional<std::array<float, 3>> Origin(GameServices& services, void* entity);

}  // namespace modlock::wasm
