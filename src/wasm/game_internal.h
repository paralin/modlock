#pragma once

#include <array>
#include <expected>
#include <optional>
#include <string>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/hero_definitions.h"
#include "modlock/gameinterop/native_memory.h"
#include "src/wasm/wasm.pb.h"

// Helpers the translation units of Game and GameServices share.
namespace modlock::wasm {

// kMaxChanges bounds the button changes, and separately the launches, one
// mod has not yet received.
inline constexpr size_t kMaxChanges = 256;

inline std::array<float, 3> Floats(const geometry::Vec3& vector) {
  return {static_cast<float>(vector.x()), static_cast<float>(vector.y()),
          static_cast<float>(vector.z())};
}

inline std::array<float, 3> Floats(const geometry::EulerAngles& angles) {
  return {static_cast<float>(angles.pitch()), static_cast<float>(angles.yaw()),
          static_cast<float>(angles.roll())};
}

inline void SetAngles(geometry::EulerAngles* out, const std::array<float, 3>& angles) {
  out->set_pitch(angles[0]);
  out->set_yaw(angles[1]);
  out->set_roll(angles[2]);
}

inline void SetVector(geometry::Vec3* out, const std::array<float, 3>& vector) {
  out->set_x(vector[0]);
  out->set_y(vector[1]);
  out->set_z(vector[2]);
}

// HitEntity is what a trace or impact reports as struck: the entity's handle,
// or zero for the world, entity index 0, and for nothing.
inline uint32_t HitEntity(const std::optional<uint32_t>& handle) {
  return handle && gameinterop::EntityIndexOf(*handle) != 0 ? *handle : 0;
}

// kMaxList bounds the elements of a list field the host reads or grows to, so
// a list the game has not initialized cannot size a huge copy.
inline constexpr int32_t kMaxList = 1 << 20;

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
inline std::expected<ListHeader, std::string> ReadList(const unsigned char* address) {
  ListHeader list;
  if (!gameinterop::ReadNative(address, &list, sizeof(list))) {
    return std::unexpected("cannot read the list");
  }
  if (list.size < 0 || list.size > list.capacity || list.capacity > kMaxList) {
    return std::unexpected("the list is not initialized");
  }
  return list;
}

// FindHero resolves a request's hero by its identifier or its name.
template <typename Request>
std::expected<gameinterop::HeroDefinition, std::string> FindHero(
    const gameinterop::HeroDefinitions& heroes, const Request& request) {
  if (request.hero_case() == Request::kHeroId) {
    return heroes.Find(request.hero_id());
  }
  return heroes.Find(request.hero_name());
}

}  // namespace modlock::wasm
