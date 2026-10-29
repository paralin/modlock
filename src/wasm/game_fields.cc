//go:build deps_only

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "modlock/gameinterop/native_memory.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

namespace {

// kMaxString bounds the bytes ReadField copies for a string field.
constexpr size_t kMaxString = 256;

// kMaxList bounds the elements of a list field the host reads or grows to, so
// a list the game has not initialized cannot size a huge copy.
constexpr int32_t kMaxList = 1 << 20;

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

// ListHeader is the layout of a list field: a CUtlVector, which the game's
// networked lists share. The elements are an array of capacity entries.
struct ListHeader {
  int32_t size;
  int32_t padding;
  void* elements;
  int32_t capacity;
  int32_t grow;
};

static_assert(sizeof(ListHeader) == 24);

// ReadList copies the header of the list field at address.
std::expected<ListHeader, std::string> ReadList(const unsigned char* address) {
  ListHeader list;
  if (!gameinterop::ReadNative(address, &list, sizeof(list))) {
    return std::unexpected("cannot read the list");
  }
  if (list.size < 0 || list.size > list.capacity || list.capacity > kMaxList) {
    return std::unexpected("the list is not initialized");
  }
  return list;
}

// Size returns the bytes a field type occupies in the entity.
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

// Decode converts the bytes of one field to its value.
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

// Interned returns a copy of text that lives for the life of the process, one
// copy per distinct text. A symbol field points to it: the entity keeps the
// pointer past the mod that wrote it and never frees it, as it never frees the
// game's own interned symbols.
const char* Interned(std::string text) {
  static auto* texts = new std::unordered_set<std::string>();
  return texts->insert(std::move(text)).first->c_str();
}

// Encode converts a value to the bytes of one field of type. The value must
// take the form the type reads as. Text is not encoded here, since its field
// points to the characters; Game::StoreField writes a symbol field itself.
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

// ReadValue copies the field of type at address through the operating system,
// so a field the object lacks fails instead of faulting the server, and
// decodes it. Name names the field in an error.
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

// Holds checks that field's storage holds a value of type, or a list of them
// when list is set, as the schema declares it, so a mod reads and writes only
// the values the game keeps there and never a pointer or a structure. A
// refusal names the field as class_name.field_name.
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

// ModifierName copies the name of the modifier at modifier: its definition,
// at +0x10 as in Deadworks CBaseModifier, keeps its interned name at +0x10, as
// every CEntitySubclassVDataBase does. An unreadable name is empty.
std::string ModifierName(const unsigned char* modifier) {
  const unsigned char* definition = nullptr;
  const char* interned = nullptr;
  if (!gameinterop::ReadNative(modifier + 0x10, &definition, sizeof(definition)) || !definition ||
      !gameinterop::ReadNative(definition + 0x10, &interned, sizeof(interned)) || !interned) {
    return {};
  }
  std::array<char, 128> name{};
  if (!gameinterop::ReadNative(interned, name.data(), name.size() - 1)) {
    return {};
  }
  return name.data();
}

}  // namespace

std::expected<unsigned char*, std::string> Game::FieldAddress(void* entity,
                                                              const std::string& class_name,
                                                              const std::string& field,
                                                              fields::FieldType type, bool list) {
  // Resolve the field's offset and check it holds the requested type.
  auto found = services_.Field(class_name, field);
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
  if (IsRulesClass(class_name) && services_.Derives(*actual, "CGameRulesProxy")) {
    auto rules = services_.Rules();
    if (!rules) {
      return std::unexpected(rules.error());
    }
    return static_cast<unsigned char*>(*rules) + found->offset;
  }
  if (IsModelClass(class_name)) {
    auto part = ModelPart(entity, *actual, class_name);
    if (!part) {
      return std::unexpected(part.error());
    }
    return *part + found->offset;
  }
  if (!services_.Derives(*actual, class_name)) {
    return std::unexpected("the entity is a " +
                           std::string(gameinterop::SchemaClassNameOf(*actual)) + ", not a " +
                           class_name);
  }
  return static_cast<unsigned char*>(entity) + found->offset;
}

std::expected<unsigned char*, std::string> Game::ModelPart(void* entity, const void* actual,
                                                           const std::string& class_name) {
  // An animated model owns the legacy controller in its specialized body component.
  if (class_name == "CBaseAnimGraphController" && !services_.Derives(actual, "CBaseAnimGraph")) {
    return std::unexpected("the entity has no animation controller");
  }

  // Only a model entity's scene node is a skeleton instance holding a model
  // state; any entity has a scene node.
  if (class_name != "CGameSceneNode" && !services_.Derives(actual, "CBaseModelEntity")) {
    return std::unexpected("the entity is a " +
                           std::string(gameinterop::SchemaClassNameOf(actual)) + ", which has no " +
                           class_name);
  }
  auto body = services_.Field("CBaseEntity", "m_CBodyComponent");
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
    auto controller = services_.Field("CBodyComponentBaseAnimGraph", "m_animationController");
    if (!controller) {
      return std::unexpected(controller.error());
    }
    return component + controller->offset;
  }

  // Scene, skeleton and model state fields use the body's scene node.
  auto node = services_.Field("CBodyComponent", "m_pSceneNode");
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
  auto state = services_.Field("CSkeletonInstance", "m_modelState");
  if (!state) {
    return std::unexpected(state.error());
  }
  return scene + state->offset;
}

std::expected<void, std::string> Game::StoreField(void* entity, const std::string& class_name,
                                                  const std::string& field, fields::FieldType type,
                                                  const fields::FieldValue& value) {
  // Find the field the value goes to.
  auto address = FieldAddress(entity, class_name, field, type, false);
  if (!address) {
    return std::unexpected(address.error());
  }

  // Text goes only to a symbol field, as a pointer to an interned copy with
  // the mod's content addon in place of {addon}. A CUtlString owns its
  // characters, and the game would free a pointer it did not allocate.
  if (type == fields::FIELD_TYPE_STRING) {
    auto found = services_.Field(class_name, field);
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
    const char* text = Interned(WithAddon(value.text()));
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

std::expected<fields::FieldResponse, std::string> Game::ReadField(
    const fields::ReadFieldRequest& request) {
  // Find the entity and the field's address.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  if (Size(request.type()) == 0) {
    return std::unexpected("the field type is unknown");
  }
  auto address =
      FieldAddress(*entity, request.class_name(), request.field(), request.type(), false);
  if (!address) {
    return std::unexpected(address.error());
  }
  return ReadValue(*address, request.type(), request.class_name() + "." + request.field());
}

std::expected<fields::FieldResponse, std::string> Game::ReadModifierField(
    const fields::ReadModifierFieldRequest& request) {
  // Find the entity, the field and the entity's modifier list.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  if (Size(request.type()) == 0) {
    return std::unexpected("the field type is unknown");
  }
  auto found = services_.Field(request.class_name(), request.field());
  if (!found) {
    return std::unexpected(found.error());
  }
  if (auto holds = Holds(*found, request.type(), false, request.class_name(), request.field());
      !holds) {
    return std::unexpected(holds.error());
  }
  auto layout = services_.Modifiers();
  if (!layout) {
    return std::unexpected(layout.error());
  }
  const unsigned char* property = nullptr;
  if (!gameinterop::ReadNative(static_cast<unsigned char*>(*entity) + layout->property, &property,
                               sizeof(property))) {
    return std::unexpected("cannot read the entity's modifiers");
  }
  if (!property) {
    return fields::FieldResponse{};
  }
  auto list = ReadList(property + layout->list);
  if (!list) {
    return std::unexpected(list.error());
  }

  // Read the field of the first modifier with the name; without one, answer
  // no value.
  std::vector<unsigned char*> modifiers(static_cast<size_t>(list->size));
  if (!modifiers.empty() && !gameinterop::ReadNative(list->elements, modifiers.data(),
                                                     modifiers.size() * sizeof(unsigned char*))) {
    return std::unexpected("cannot read the entity's modifiers");
  }
  for (auto* modifier : modifiers) {
    if (modifier && ModifierName(modifier) == request.modifier()) {
      return ReadValue(modifier + found->offset, request.type(),
                       request.modifier() + "." + request.field());
    }
  }
  return fields::FieldResponse{};
}

std::expected<void, std::string> Game::WriteField(const fields::WriteFieldRequest& request) {
  // Find the entity and store the value in its field.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto stored =
      StoreField(*entity, request.class_name(), request.field(), request.type(), request.value());
  if (!stored) {
    return stored;
  }

  // Tell the engine to send the changed field.
  if (!gameinterop::NotifyEntityStateChanged(*entity)) {
    return std::unexpected("cannot replicate " + request.class_name() + "." + request.field());
  }
  return {};
}

std::expected<fields::FieldListResponse, std::string> Game::ReadFieldList(
    const fields::ReadFieldRequest& request) {
  // Find the entity, the list field's address and its header.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto stride = Size(request.type());
  if (stride == 0) {
    return std::unexpected("the field type is unknown");
  }
  auto address = FieldAddress(*entity, request.class_name(), request.field(), request.type(), true);
  if (!address) {
    return std::unexpected(address.error());
  }
  auto list = ReadList(*address);
  if (!list) {
    return std::unexpected(list.error());
  }

  // Copy every element through the operating system, then decode each.
  std::vector<unsigned char> bytes(static_cast<size_t>(list->size) * stride);
  if (!bytes.empty() && !gameinterop::ReadNative(list->elements, bytes.data(), bytes.size())) {
    return std::unexpected("cannot read " + request.class_name() + "." + request.field());
  }
  fields::FieldListResponse response;
  for (size_t at = 0; at < bytes.size(); at += stride) {
    auto value = Decode(request.type(), &bytes[at]);
    if (!value) {
      return std::unexpected(value.error());
    }
    *response.add_values() = std::move(*value);
  }
  return response;
}

std::expected<void, std::string> Game::WriteFieldList(
    const fields::WriteFieldListRequest& request) {
  // Find the entity and encode every value.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto stride = Size(request.type());
  if (stride == 0) {
    return std::unexpected("the field type is unknown");
  }
  if (request.values_size() > kMaxList) {
    return std::unexpected("the list is too long");
  }
  std::vector<unsigned char> bytes;
  for (const auto& value : request.values()) {
    auto encoded = Encode(request.type(), value);
    if (!encoded) {
      return std::unexpected(encoded.error());
    }
    bytes.insert(bytes.end(), encoded->begin(), encoded->begin() + stride);
  }

  // Find the list field's address and its header.
  auto address = FieldAddress(*entity, request.class_name(), request.field(), request.type(), true);
  if (!address) {
    return std::unexpected(address.error());
  }
  auto list = ReadList(*address);
  if (!list) {
    return std::unexpected(list.error());
  }

  // A list that is too short takes a larger buffer from the game's allocator,
  // which the game frees with the list. The old buffer is left to the game
  // since its owner is unknown, and doubling bounds what that leaves behind.
  const auto count = request.values_size();
  if (count > list->capacity) {
    // Grow the list, at least doubling it.
    const int32_t capacity = std::clamp(2 * list->capacity, count, kMaxList);
    void* grown = gameinterop::AllocateNative(static_cast<size_t>(capacity) * stride);
    if (grown == nullptr) {
      return std::unexpected("cannot grow " + request.field());
    }
    list->elements = grown;
    list->capacity = capacity;
  }

  // Fill the elements before the header, so the list never counts an element
  // that is not there.
  const std::string name = request.class_name() + "." + request.field();
  if (!bytes.empty() && !gameinterop::WriteNative(list->elements, bytes.data(), bytes.size())) {
    return std::unexpected("cannot write " + name);
  }
  list->size = count;
  if (!gameinterop::WriteNative(*address, &*list, sizeof(*list))) {
    return std::unexpected("cannot write " + name);
  }
  if (!gameinterop::NotifyEntityStateChanged(*entity)) {
    return std::unexpected("cannot replicate " + name);
  }
  return {};
}

std::expected<entities::EntityClassResponse, std::string> Game::EntityClass(
    const entities::EntityRequest& request) {
  // Find the entity and its schema class.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto schema = gameinterop::SchemaClassOfEntity(*entity, gameinterop::ReadNative);
  if (!schema) {
    return std::unexpected(schema.error());
  }

  // Answer the class and designer names.
  entities::EntityClassResponse response;
  auto* name = response.mutable_name();
  name->set_class_name(std::string(gameinterop::SchemaClassNameOf(*schema)));
  name->set_designer_name(std::string(gameinterop::DesignerName(*entity)));
  return response;
}

std::expected<entities::EntityNameResponse, std::string> Game::EntityName(
    const entities::EntityRequest& request) {
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  entities::EntityNameResponse response;
  response.set_name(std::string(gameinterop::EntityName(*entity)));
  return response;
}

std::expected<entities::EntityPositionResponse, std::string> Game::EntityPosition(
    const entities::EntityRequest& request) {
  // Find the entity and read its scene node's origin and angles.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto origin = SceneVector(*entity, "m_vecAbsOrigin");
  const auto angles = SceneVector(*entity, "m_angAbsRotation");
  if (!origin || !angles) {
    return std::unexpected("the entity has no place in the world");
  }

  // Answer them, with the velocity when the engine's call resolves.
  entities::EntityPositionResponse response;
  auto* placement = response.mutable_placement();
  SetVector(placement->mutable_position(), *origin);
  SetAngles(placement->mutable_facing(), *angles);
  if (auto dynamics = services_.Dynamics()) {
    std::array<float, 3> velocity{};
    (*dynamics)->velocity(*entity, velocity.data());
    SetVector(placement->mutable_velocity(), velocity);
  }
  return response;
}

std::expected<abilities::ActiveResponse, std::string> Game::ModifierState(
    const abilities::ModifierStateRequest& request) {
  // Find the entity and the mask word that holds the state.
  auto entity = Entity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto word = FindModifierWord(*entity, request.state());
  if (!word) {
    return std::unexpected(word.error());
  }
  abilities::ActiveResponse response;
  if (!*word) {
    return response;
  }

  // The game predicts some states, such as parry, so a state is active when
  // either mask holds it.
  uint32_t ordinary = 0;
  uint32_t predicted = 0;
  if (!gameinterop::ReadNative((*word)->ordinary, &ordinary, sizeof(ordinary)) ||
      !gameinterop::ReadNative((*word)->predicted, &predicted, sizeof(predicted))) {
    return std::unexpected("cannot read the entity's modifier states");
  }
  response.set_active(((ordinary | predicted) & (*word)->bit) != 0);
  return response;
}

std::expected<void, std::string> Game::HoldModifierState(
    const abilities::HoldModifierStateRequest& request) {
  // Releasing a hold clears the state the hold set.
  auto hold = std::pair(request.entity(), request.state());
  if (request.has_active() && !request.active()) {
    if (held_.erase(hold) == 0) {
      return {};
    }
    return SetModifierState(request.entity(), request.state(), false);
  }

  // Holding sets the state now and every frame after.
  if (auto set = SetModifierState(request.entity(), request.state(), true); !set) {
    return set;
  }
  held_.insert(std::move(hold));
  return {};
}

void Game::StartFreeForAll(bool enabled) {
  free_for_all_ = enabled;
  team_seating_ = false;
  if (free_for_all_) {
    EnableRelations();
  }
}

void Game::EnableRelations() {
  // Without the relations only bullets reach teammates; the match still runs.
  if (auto relations = services_.Relations()) {
    (*relations)->SetEnabled(true);
  } else {
    std::cerr << "free-for-all relations: " << relations.error() << '\n';
  }
}

std::expected<void, std::string> Game::SetFreeForAll(const match::SetFreeForAllRequest& request) {
  if (free_for_all_ == request.enabled()) {
    return {};
  }
  free_for_all_ = request.enabled();
  if (free_for_all_) {
    EnableRelations();
  } else {
    ReleaseFreeForAll();
  }
  return {};
}

std::expected<void, std::string> Game::SetTeamSeating(const match::SetTeamSeatingRequest& request) {
  team_seating_ = request.enabled();
  return {};
}

bool Game::FreeForAllCommand(std::string_view line) const {
  // Only changeteam is blocked, and only in free-for-all.
  if (!free_for_all_) {
    return false;
  }
  const auto name = line.substr(0, line.find_first_of(" \t"));
  return name == "changeteam";
}

void Game::SeatPlayers() {
  // Seat players only while seating or free-for-all is on.
  if (!free_for_all_ && !team_seating_) {
    return;
  }
  auto selection = services_.Selection();
  if (selection) {
    // Seat each player with no team on the side with fewer players.
    std::array<int, 4> sides{};
    std::vector<int32_t> unseated;
    for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
      // Count each side and collect the humans without a team.
      const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
      if (!state.occupied) {
        continue;
      }
      const auto team = observer_.ControllerTeam(slot);
      if (!team) {
        continue;
      }
      if (*team == 0 && !state.is_bot && state.xuid != 0) {
        unseated.push_back(slot);
      } else if (*team < sides.size()) {
        ++sides[*team];
      }
    }
    for (const auto slot : unseated) {
      const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
      const int team = sides[3] < sides[2] ? 3 : 2;
      auto seated = observer_.SeatForChoice(slot, state.xuid, state.generation, team, **selection);
      if (seated && *seated) {
        ++sides[team];
        seat_failed_.erase(slot);
      } else if (!seated && seat_failed_.insert(slot).second) {
        std::cerr << "team seating: " << seated.error() << '\n';
      }
    }
  } else if (seat_failed_.insert(-1).second) {
    std::cerr << "team seating: " << selection.error() << '\n';
  }
}

void Game::ApplyFreeForAll() {
  // Hold friendly fire only in free-for-all.
  if (!free_for_all_) {
    return;
  }
  static constexpr const char* kFriendlyFire = "MODIFIER_STATE_FRIENDLY_FIRE_ENABLED";

  // Hold friendly fire on each living hero, and let a gone hero's hold go.
  std::set<uint32_t> living;
  for (const auto& hero : Heroes()) {
    living.insert(hero.handle);
    if (ffa_held_.insert(hero.handle).second) {
      held_.insert({hero.handle, kFriendlyFire});
    }
  }
  std::erase_if(ffa_held_, [&](uint32_t handle) {
    if (living.contains(handle)) {
      return false;
    }
    held_.erase({handle, kFriendlyFire});
    return true;
  });
}

void Game::ReleaseFreeForAll() {
  if (services_.relations_) {
    services_.relations_->SetEnabled(false);
  }
  static constexpr const char* kFriendlyFire = "MODIFIER_STATE_FRIENDLY_FIRE_ENABLED";
  for (const auto handle : ffa_held_) {
    if (held_.erase({handle, kFriendlyFire}) != 0) {
      static_cast<void>(SetModifierState(handle, kFriendlyFire, false));
    }
  }
  ffa_held_.clear();
  seat_failed_.clear();
}

std::expected<std::optional<Game::ModifierWord>, std::string> Game::FindModifierWord(
    void* entity, const std::string& state) {
  // Resolve the mask layout and the state's bit.
  auto layout = services_.Modifiers();
  if (!layout) {
    return std::unexpected(layout.error());
  }
  auto value = services_.ModifierState(state);
  if (!value) {
    return std::unexpected(value.error());
  }
  if (*value < 0 || static_cast<size_t>(*value) >= layout->mask_size * 8) {
    return std::unexpected(state + " is outside the modifier state masks");
  }

  // Read the entity's modifier property.
  unsigned char* property = nullptr;
  if (!gameinterop::ReadNative(static_cast<const unsigned char*>(entity) + layout->property,
                               &property, sizeof(property))) {
    return std::unexpected("cannot read the entity's modifiers");
  }
  if (property == nullptr) {
    return std::nullopt;
  }

  // Point at the word that holds the state in each mask.
  const auto word = static_cast<size_t>(*value / 32) * sizeof(uint32_t);
  return ModifierWord{
      .ordinary = property + layout->mask + word,
      .predicted = property + layout->predicted_mask + word,
      .bit = uint32_t{1} << (*value % 32),
  };
}

std::expected<void, std::string> Game::SetModifierState(uint32_t handle, const std::string& state,
                                                        bool active) {
  // Find the entity and the mask word that holds the state.
  auto entity = Entity(handle);
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto word = FindModifierWord(*entity, state);
  if (!word) {
    return std::unexpected(word.error());
  }
  if (!*word) {
    return std::unexpected("the entity has no modifiers");
  }

  // Read the ordinary mask and set or clear the state's bit.
  uint32_t current = 0;
  if (!gameinterop::ReadNative((*word)->ordinary, &current, sizeof(current))) {
    return std::unexpected("cannot read the entity's modifier states");
  }
  const uint32_t desired = active ? current | (*word)->bit : current & ~(*word)->bit;
  if (desired == current) {
    return {};
  }

  // Write the mask back and send it.
  if (!gameinterop::WriteNative((*word)->ordinary, &desired, sizeof(desired))) {
    return std::unexpected("cannot write the entity's modifier states");
  }
  if (!gameinterop::NotifyEntityStateChanged(*entity)) {
    return std::unexpected("cannot replicate the entity's modifier states");
  }
  return {};
}

}  // namespace modlock::wasm
