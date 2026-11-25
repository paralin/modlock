//go:build deps_only

#include "wasm/entity_fields.h"

#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "modlock/gameinterop/native_memory.h"
#include "wasm/entity_rules.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"
#include "wasm/text_values.h"

namespace modlock::wasm {

namespace {

// kMaxString bounds the bytes ReadField copies for a string field.
constexpr size_t kMaxString = 256;

// IsRulesClass reports whether class_name is one of the classes that declare
// the game rules' fields.
bool IsRulesClass(const std::string& class_name) {
  return class_name == "CGameRules" || class_name == "CTeamplayRules" ||
         class_name == "CCitadelGameRules";
}

// IsModelClass reports whether class_name is a part of an entity's model that
// its body component holds: the scene node, a model entity's skeleton
// instance, model state, or legacy animation controller. The entity sends
// their fields.
bool IsModelClass(const std::string& class_name) {
  return class_name == "CGameSceneNode" || class_name == "CSkeletonInstance" ||
         class_name == "CModelState" || class_name == "CBaseAnimGraphController";
}

// MovementPawn returns the pawn class whose movement services class_name is,
// or an empty string when class_name is not a movement services class. The
// pawn's m_pMovementServices points to the services, which no entity holds.
std::string_view MovementPawn(const std::string& class_name) {
  if (class_name == "CPlayer_MovementServices") {
    return "CBasePlayerPawn";
  }
  if (class_name == "CCitadelPlayer_MovementServices") {
    return "CCitadelPlayerPawn";
  }
  if (class_name == "CCitadelObserver_MovementServices") {
    return "CCitadelObserverPawn";
  }
  return {};
}

// MovementServices returns the address of the field at offset in the
// movement services of entity, a pawn that must derive pawn_class. actual
// is the entity's schema class.
std::expected<unsigned char*, std::string> MovementServices(GameServices& services, void* entity,
                                                            const void* actual,
                                                            const std::string& pawn_class,
                                                            size_t offset) {
  if (!services.Derives(actual, pawn_class)) {
    return std::unexpected("the entity is a " +
                           std::string(gameinterop::SchemaClassNameOf(actual)) + ", not a " +
                           pawn_class);
  }
  auto field = services.Field("CBasePlayerPawn", "m_pMovementServices");
  if (!field) {
    return std::unexpected(field.error());
  }
  unsigned char* movement = nullptr;
  std::memcpy(&movement, static_cast<unsigned char*>(entity) + field->offset, sizeof(movement));
  if (movement == nullptr) {
    return std::unexpected("the pawn has no movement services");
  }
  return movement + offset;
}

// ModelPart returns the address of the part of entity's model that
// class_name names, as IsModelClass lists, through its body component,
// including CBaseAnimGraphController for animated model entities.
// actual is the entity's schema class.
std::expected<unsigned char*, std::string> ModelPart(GameServices& services, void* entity,
                                                     const void* actual,
                                                     const std::string& class_name) {
  // An animated model owns the legacy controller in its specialized body component.
  if (class_name == "CBaseAnimGraphController" && !services.Derives(actual, "CBaseAnimGraph")) {
    return std::unexpected("the entity has no animation controller");
  }

  // Only a model entity's scene node is a skeleton instance holding a model
  // state; any entity has a scene node.
  if (class_name != "CGameSceneNode" && !services.Derives(actual, "CBaseModelEntity")) {
    return std::unexpected("the entity is a " +
                           std::string(gameinterop::SchemaClassNameOf(actual)) + ", which has no " +
                           class_name);
  }
  auto body = services.Field("CBaseEntity", "m_CBodyComponent");
  if (!body) {
    return std::unexpected(body.error());
  }
  // Resolve the live body component before selecting its embedded controller or scene node.
  unsigned char* component = nullptr;
  if (!gameinterop::ReadNative(static_cast<unsigned char*>(entity) + body->offset, &component,
                               sizeof(component)) ||
      component == nullptr) {
    return std::unexpected("the entity has no body component");
  }
  if (class_name == "CBaseAnimGraphController") {
    auto controller = services.Field("CBodyComponentBaseAnimGraph", "m_animationController");
    if (!controller) {
      return std::unexpected(controller.error());
    }
    return component + controller->offset;
  }

  // Scene, skeleton and model state fields use the body's scene node.
  auto node = services.Field("CBodyComponent", "m_pSceneNode");
  if (!node) {
    return std::unexpected(node.error());
  }
  unsigned char* scene = nullptr;
  if (!gameinterop::ReadNative(component + node->offset, &scene, sizeof(scene)) ||
      scene == nullptr) {
    return std::unexpected("the entity has no scene node");
  }
  if (class_name != "CModelState") {
    return scene;
  }

  // The skeleton instance embeds the model state.
  auto state = services.Field("CSkeletonInstance", "m_modelState");
  if (!state) {
    return std::unexpected(state.error());
  }
  return scene + state->offset;
}

}  // namespace

size_t Size(fields::FieldType type) {
  switch (type) {
    case fields::FIELD_TYPE_BOOL:
    case fields::FIELD_TYPE_INT8:
    case fields::FIELD_TYPE_UINT8:
      return 1;
    case fields::FIELD_TYPE_INT16:
    case fields::FIELD_TYPE_UINT16:
      return 2;
    case fields::FIELD_TYPE_INT32:
    case fields::FIELD_TYPE_UINT32:
    case fields::FIELD_TYPE_FLOAT32:
    case fields::FIELD_TYPE_HANDLE:
      return 4;
    case fields::FIELD_TYPE_INT64:
    case fields::FIELD_TYPE_UINT64:
    case fields::FIELD_TYPE_FLOAT64:
    case fields::FIELD_TYPE_STRING:
      return 8;
    case fields::FIELD_TYPE_VECTOR:
      return 12;
    default:
      return 0;
  }
}

std::expected<fields::FieldValue, std::string> Decode(fields::FieldType type,
                                                      const unsigned char* bytes) {
  // Copy a scalar of type T out of the field's bytes.
  auto as = [bytes]<typename T>(T) {
    T value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
  };
  fields::FieldValue value;
  switch (type) {
    case fields::FIELD_TYPE_BOOL:
      value.set_boolean(bytes[0] != 0);
      break;
    case fields::FIELD_TYPE_INT8:
      value.set_number(as(int8_t{}));
      break;
    case fields::FIELD_TYPE_INT16:
      value.set_number(as(int16_t{}));
      break;
    case fields::FIELD_TYPE_INT32:
      value.set_number(as(int32_t{}));
      break;
    case fields::FIELD_TYPE_UINT8:
      value.set_number(as(uint8_t{}));
      break;
    case fields::FIELD_TYPE_UINT16:
      value.set_number(as(uint16_t{}));
      break;
    case fields::FIELD_TYPE_UINT32:
      value.set_number(as(uint32_t{}));
      break;
    case fields::FIELD_TYPE_INT64:
    case fields::FIELD_TYPE_UINT64:
      // An unsigned value travels as the signed integer of the same bits.
      value.set_integer(as(int64_t{}));
      break;
    case fields::FIELD_TYPE_FLOAT32:
      value.set_number(as(float{}));
      break;
    case fields::FIELD_TYPE_FLOAT64:
      value.set_number(as(double{}));
      break;
    case fields::FIELD_TYPE_VECTOR: {
      const auto vector = as(std::array<float, 3>{});
      value.mutable_vector()->set_x(vector[0]);
      value.mutable_vector()->set_y(vector[1]);
      value.mutable_vector()->set_z(vector[2]);
      break;
    }
    case fields::FIELD_TYPE_HANDLE:
      value.set_number(as(uint32_t{}));
      break;
    case fields::FIELD_TYPE_STRING: {
      // The field points to the characters; an empty pointer is empty text.
      const auto* text = as(static_cast<const char*>(nullptr));
      if (text == nullptr) {
        value.set_text("");
        break;
      }
      std::array<char, kMaxString> copy{};
      size_t length = 0;
      while (length < copy.size() && gameinterop::ReadNative(text + length, &copy[length], 1) &&
             copy[length] != '\0') {
        ++length;
      }
      value.set_text(std::string(copy.data(), length));
      break;
    }
    default:
      return std::unexpected("the field type is unknown");
  }
  return value;
}

std::expected<std::array<unsigned char, 12>, std::string> Encode(fields::FieldType type,
                                                                 const fields::FieldValue& value) {
  // Copy a scalar of type T into the field's bytes.
  std::array<unsigned char, 12> bytes{};
  auto put = [&bytes]<typename T>(T scalar) {
    std::memcpy(bytes.data(), &scalar, sizeof(scalar));
  };

  // Check that the value takes the form the field type reads as.
  auto expect = [&value](fields::FieldValue::ValueCase form) -> std::expected<void, std::string> {
    if (value.value_case() != form) {
      return std::unexpected("the value does not fit the field type");
    }
    return {};
  };

  // Encode the value as the field type's bytes.
  std::expected<void, std::string> fits;
  const double number = value.number();
  switch (type) {
    case fields::FIELD_TYPE_BOOL:
      fits = expect(fields::FieldValue::kBoolean);
      bytes[0] = value.boolean() ? 1 : 0;
      break;
    case fields::FIELD_TYPE_INT8:
      fits = expect(fields::FieldValue::kNumber);
      put(static_cast<int8_t>(number));
      break;
    case fields::FIELD_TYPE_INT16:
      fits = expect(fields::FieldValue::kNumber);
      put(static_cast<int16_t>(number));
      break;
    case fields::FIELD_TYPE_INT32:
      fits = expect(fields::FieldValue::kNumber);
      put(static_cast<int32_t>(number));
      break;
    case fields::FIELD_TYPE_UINT8:
      fits = expect(fields::FieldValue::kNumber);
      put(static_cast<uint8_t>(number));
      break;
    case fields::FIELD_TYPE_UINT16:
      fits = expect(fields::FieldValue::kNumber);
      put(static_cast<uint16_t>(number));
      break;
    case fields::FIELD_TYPE_UINT32:
      fits = expect(fields::FieldValue::kNumber);
      put(static_cast<uint32_t>(number));
      break;
    case fields::FIELD_TYPE_INT64:
    case fields::FIELD_TYPE_UINT64:
      fits = expect(fields::FieldValue::kInteger);
      put(value.integer());
      break;
    case fields::FIELD_TYPE_FLOAT32:
      fits = expect(fields::FieldValue::kNumber);
      put(static_cast<float>(number));
      break;
    case fields::FIELD_TYPE_FLOAT64:
      fits = expect(fields::FieldValue::kNumber);
      put(number);
      break;
    case fields::FIELD_TYPE_VECTOR:
      fits = expect(fields::FieldValue::kVector);
      put(Floats(value.vector()));
      break;
    case fields::FIELD_TYPE_HANDLE:
      fits = expect(fields::FieldValue::kNumber);
      put(static_cast<uint32_t>(number));
      break;
    case fields::FIELD_TYPE_STRING:
      return std::unexpected("a list of text cannot be written");
    default:
      return std::unexpected("the field type is unknown");
  }
  if (!fits) {
    return std::unexpected(fits.error());
  }
  return bytes;
}

std::expected<fields::FieldResponse, std::string> ReadValue(const unsigned char* address,
                                                            fields::FieldType type,
                                                            const std::string& name) {
  std::array<unsigned char, 12> bytes{};
  if (!gameinterop::ReadNative(address, bytes.data(), Size(type))) {
    return std::unexpected("cannot read " + name);
  }
  auto value = Decode(type, bytes.data());
  if (!value) {
    return std::unexpected(value.error());
  }
  fields::FieldResponse response;
  *response.mutable_value() = std::move(*value);
  return response;
}

std::expected<void, std::string> Holds(const gameinterop::SchemaField& field,
                                       fields::FieldType type, bool list,
                                       const std::string& class_name,
                                       const std::string& field_name) {
  // Accept a field that spans the type and holds the storage it reads.
  using gameinterop::SchemaStorage;
  const bool spans = field.size >= (list ? sizeof(ListHeader) : Size(type));
  const auto wanted = type == fields::FIELD_TYPE_VECTOR   ? SchemaStorage::kVector
                      : type == fields::FIELD_TYPE_STRING ? SchemaStorage::kText
                                                          : SchemaStorage::kNumber;
  if (spans && field.storage == wanted && field.list == list) {
    return {};
  }

  // Say what the field holds.
  const auto name = class_name + "." + field_name;
  if (!spans) {
    return std::unexpected(name + " is smaller than its requested type");
  }
  const std::string declared = field.type != nullptr ? field.type : "type the schema does not name";
  if (field.storage == SchemaStorage::kOpaque) {
    return std::unexpected(name + " is a " + declared +
                           ", which holds no value a mod can read or write");
  }
  static constexpr std::array<const char*, 4> kOne{"", "a number", "a vector", "text"};
  static constexpr std::array<const char*, 4> kMany{"", "numbers", "vectors", "text"};
  const auto storage = static_cast<size_t>(field.storage);
  return std::unexpected(name + " is a " + declared + ", which reads as " +
                         (field.list ? std::string("a list of ") + kMany[storage] : kOne[storage]));
}

std::expected<unsigned char*, std::string> FieldAddress(GameServices& services, void* entity,
                                                        const std::string& class_name,
                                                        const std::string& field,
                                                        fields::FieldType type, bool list) {
  // Resolve the field's offset and check it holds the requested type.
  auto found = services.Field(class_name, field);
  if (!found) {
    return std::unexpected(found.error());
  }
  if (auto holds = Holds(*found, type, list, class_name, field); !holds) {
    return std::unexpected(holds.error());
  }

  // The offset belongs to class_name, so the entity must be one.
  auto actual = gameinterop::SchemaClassOfEntity(entity, gameinterop::ReadNative);
  if (!actual) {
    return std::unexpected(actual.error());
  }

  // The game rules are not an entity: their proxy entity points to them and
  // sends their fields, so a rules field is read and written through it.
  if (IsRulesClass(class_name) && services.Derives(*actual, "CGameRulesProxy")) {
    auto rules = services.Rules();
    if (!rules) {
      return std::unexpected(rules.error());
    }
    return static_cast<unsigned char*>(*rules) + found->offset;
  }
  if (IsModelClass(class_name)) {
    auto part = ModelPart(services, entity, *actual, class_name);
    if (!part) {
      return std::unexpected(part.error());
    }
    return *part + found->offset;
  }
  if (const auto pawn = MovementPawn(class_name); !pawn.empty()) {
    return MovementServices(services, entity, *actual, std::string(pawn), found->offset);
  }
  if (!services.Derives(*actual, class_name)) {
    return std::unexpected("the entity is a " +
                           std::string(gameinterop::SchemaClassNameOf(*actual)) + ", not a " +
                           class_name);
  }
  return static_cast<unsigned char*>(entity) + found->offset;
}

std::expected<void, std::string> StoreField(GameServices& services, void* entity,
                                            const std::string& class_name, const std::string& field,
                                            fields::FieldType type, const fields::FieldValue& value,
                                            std::string_view addon) {
  // Refuse a field that names a console line or a script, then find the
  // field the value goes to.
  if (auto refused = RefuseField(field); !refused.empty()) {
    return std::unexpected(std::move(refused));
  }
  auto address = FieldAddress(services, entity, class_name, field, type, false);
  if (!address) {
    return std::unexpected(address.error());
  }

  // Text goes only to a symbol field, as a pointer to an interned copy with
  // the mod's content addon in place of {addon}. A CUtlString owns its
  // characters, and the game would free a pointer it did not allocate.
  if (type == fields::FIELD_TYPE_STRING) {
    auto found = services.Field(class_name, field);
    if (!found) {
      return std::unexpected(found.error());
    }
    if (found->type == nullptr || std::string_view(found->type) != "CUtlSymbolLarge") {
      return std::unexpected(class_name + "." + field +
                             " owns its text, so only the game can write it");
    }
    if (value.value_case() != fields::FieldValue::kText) {
      return std::unexpected("the value does not fit the field type");
    }
    const char* text = Interned(WithAddon(value.text(), std::string(addon)));
    if (!gameinterop::WriteNative(*address, &text, sizeof(text))) {
      return std::unexpected("cannot write " + class_name + "." + field);
    }
    return {};
  }

  // Encode the value.
  auto bytes = Encode(type, value);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }

  // Write the bytes into the entity.
  if (!gameinterop::WriteNative(*address, bytes->data(), Size(type))) {
    return std::unexpected("cannot write " + class_name + "." + field);
  }
  return {};
}

std::expected<fields::FieldResponse, std::string> ReadEntityField(
    GameServices& services, const fields::ReadFieldRequest& request) {
  // Find the entity and the field's address.
  auto entity = LiveEntity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  if (Size(request.type()) == 0) {
    return std::unexpected("the field type is unknown");
  }
  auto address =
      FieldAddress(services, *entity, request.class_name(), request.field(), request.type(), false);
  if (!address) {
    return std::unexpected(address.error());
  }
  return ReadValue(*address, request.type(), request.class_name() + "." + request.field());
}

unsigned char* SceneNode(GameServices& services, void* entity) {
  // Resolve the body component's fields.
  auto body = services.Field("CBaseEntity", "m_CBodyComponent");
  auto node = services.Field("CBodyComponent", "m_pSceneNode");
  if (!body || !node) {
    return nullptr;
  }

  // Follow the body component to its scene node.
  unsigned char* component = nullptr;
  std::memcpy(&component, static_cast<unsigned char*>(entity) + body->offset, sizeof(component));
  if (component == nullptr) {
    return nullptr;
  }
  unsigned char* scene = nullptr;
  std::memcpy(&scene, component + node->offset, sizeof(scene));
  return scene;
}

std::optional<std::array<float, 3>> Origin(GameServices& services, void* entity) {
  return SceneVector(services, entity, "m_vecAbsOrigin");
}

std::optional<std::array<float, 3>> SceneVector(GameServices& services, void* entity,
                                                const std::string& field) {
  // Find the scene node and the field.
  auto* scene = SceneNode(services, entity);
  auto at = services.Field("CGameSceneNode", field);
  if (scene == nullptr || !at) {
    return std::nullopt;
  }

  // Read the node's three floats.
  std::array<float, 3> value{};
  std::memcpy(value.data(), scene + at->offset, sizeof(value));
  return value;
}

}  // namespace modlock::wasm
