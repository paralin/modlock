//go:build deps_only

#include "wasm/fields.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <utility>
#include <vector>

#include "modlock/gameinterop/native_memory.h"
#include "wasm/entity_fields.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

std::expected<fields::FieldResponse, std::string> Fields::ReadField(
    const fields::ReadFieldRequest& request) {
  return ReadEntityField(services_, request);
}

std::expected<void, std::string> Fields::WriteField(const fields::WriteFieldRequest& request) {
  // Find the entity and store the value in its field.
  auto entity = LiveEntity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  auto stored = StoreField(services_, *entity, request.class_name(), request.field(),
                           request.type(), request.value(), world_.Addon());
  if (!stored) {
    return stored;
  }

  // Tell the engine to send the changed field.
  if (!gameinterop::NotifyEntityStateChanged(*entity)) {
    return std::unexpected("cannot replicate " + request.class_name() + "." + request.field());
  }

  // A client builds a client UI entity's panel only when it creates the
  // entity, so a new layout reaches connected players through a full update,
  // which creates every entity again.
  if (request.field() == "m_DialogXMLName") {
    rebuild_panels_ = true;
  }
  return {};
}

void Fields::Frame() {
  // One full update serves every layout written in the frame.
  if (!std::exchange(rebuild_panels_, false)) {
    return;
  }
  auto updates = services_.FullUpdates();
  if (!updates) {
    std::cerr << "panel layouts: " << updates.error() << '\n';
    return;
  }
  (*updates)->RequestAll();
}

std::expected<fields::FieldListResponse, std::string> Fields::ReadFieldList(
    const fields::ReadFieldRequest& request) {
  // Find the entity, the list field's address and its header.
  auto entity = LiveEntity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto stride = Size(request.type());
  if (stride == 0) {
    return std::unexpected("the field type is unknown");
  }
  auto address =
      FieldAddress(services_, *entity, request.class_name(), request.field(), request.type(), true);
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

std::expected<void, std::string> Fields::WriteFieldList(
    const fields::WriteFieldListRequest& request) {
  // Find the entity and encode every value.
  auto entity = LiveEntity(request.entity());
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
  auto address =
      FieldAddress(services_, *entity, request.class_name(), request.field(), request.type(), true);
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

std::expected<entities::EntityClassResponse, std::string> Fields::EntityClass(
    const entities::EntityRequest& request) {
  // Find the entity and its schema class.
  auto entity = LiveEntity(request.entity());
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

std::expected<entities::EntityNameResponse, std::string> Fields::EntityName(
    const entities::EntityRequest& request) {
  auto entity = LiveEntity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  entities::EntityNameResponse response;
  response.set_name(std::string(gameinterop::EntityName(*entity)));
  return response;
}

std::expected<entities::EntityPositionResponse, std::string> Fields::EntityPosition(
    const entities::EntityRequest& request) {
  // Find the entity and read its scene node's origin and angles.
  auto entity = LiveEntity(request.entity());
  if (!entity) {
    return std::unexpected(entity.error());
  }
  const auto origin = SceneVector(services_, *entity, "m_vecAbsOrigin");
  const auto angles = SceneVector(services_, *entity, "m_angAbsRotation");
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

}  // namespace modlock::wasm
