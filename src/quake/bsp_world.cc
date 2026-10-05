#include "quake/bsp_world.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>

namespace modlock::quake {
namespace {

// Read decodes the external BSP format after the enclosing lump is bounded.
template <typename T>
T Read(std::span<const std::uint8_t> bytes, std::size_t offset) {
  static_assert(std::endian::native == std::endian::little);
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

}  // namespace

std::expected<BspWorld, std::string> BspWorld::Decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 124 || Read<int32_t>(bytes, 0) != 29)
    return std::unexpected("Quake collision requires a version 29 BSP header");
  std::array<std::span<const std::uint8_t>, 15> lumps;
  for (std::size_t index = 0; index < lumps.size(); ++index) {
    const auto offset = Read<uint32_t>(bytes, 4 + index * 8);
    const auto length = Read<uint32_t>(bytes, 8 + index * 8);
    if (offset > bytes.size() || length > bytes.size() - offset)
      return std::unexpected("Quake BSP lump extends beyond the file");
    lumps[index] = bytes.subspan(offset, length);
  }
  if (lumps[1].size() % 20 || lumps[5].size() % 24 || lumps[9].size() % 8 ||
      lumps[10].size() % 28 || lumps[14].size() < 64 || lumps[14].size() % 64)
    return std::unexpected("Quake BSP collision lump has a partial record");

  BspWorld world;
  for (std::size_t offset = 0; offset < lumps[1].size(); offset += 20) {
    Plane plane{.normal = {Read<float>(lumps[1], offset), Read<float>(lumps[1], offset + 4),
                           Read<float>(lumps[1], offset + 8)},
                .distance = Read<float>(lumps[1], offset + 12),
                .axis = Read<int32_t>(lumps[1], offset + 16)};
    if (plane.axis < 0 || plane.axis > 5 || !std::isfinite(plane.distance) ||
        !std::ranges::all_of(plane.normal, [](float value) { return std::isfinite(value); }))
      return std::unexpected("Quake BSP collision plane is invalid");
    world.planes_.push_back(plane);
  }

  for (std::size_t offset = 0; offset < lumps[5].size(); offset += 24) {
    Node node{.plane = Read<int32_t>(lumps[5], offset)};
    for (std::size_t side = 0; side < 2; ++side) {
      int child = Read<int16_t>(lumps[5], offset + 4 + side * 2);
      if (child < 0) {
        const auto leaf = static_cast<std::size_t>(-child - 1);
        if (leaf >= lumps[10].size() / 28)
          return std::unexpected("Quake BSP node references a missing leaf");
        child = Read<int32_t>(lumps[10], leaf * 28);
        if (child >= 0) return std::unexpected("Quake BSP leaf has invalid contents");
      }
      node.children[side] = child;
    }
    world.hulls_[0].nodes.push_back(node);
  }
  for (std::size_t offset = 0; offset < lumps[9].size(); offset += 8) {
    world.hulls_[1].nodes.push_back(
        {.plane = Read<int32_t>(lumps[9], offset),
         .children = {Read<int16_t>(lumps[9], offset + 4), Read<int16_t>(lumps[9], offset + 6)}});
  }
  for (std::size_t index = 0; index < world.hulls_.size(); ++index) {
    auto& hull = world.hulls_[index];
    hull.root = Read<int32_t>(lumps[14], 36 + index * 4);
    if (auto valid = world.Validate(hull); !valid) return std::unexpected(valid.error());
  }
  return world;
}

std::expected<void, std::string> BspWorld::Validate(const Hull& hull) const {
  std::vector<std::uint8_t> visited(hull.nodes.size());
  std::vector<int> height(hull.nodes.size());
  std::function<bool(int, int)> visit = [&](int index, int depth) {
    if (index < 0) return index >= -6;
    // Bounded depth protects recursive sweeps from corrupt cyclic or excessively
    // deep trees. Ordinary compiled Quake maps have much shallower BSP trees.
    if (static_cast<std::size_t>(index) >= hull.nodes.size() || depth > 512) return false;
    if (visited[index] == 1) return false;
    if (visited[index] == 2) return depth + height[index] <= 512;
    const auto& node = hull.nodes[index];
    if (node.plane < 0 || static_cast<std::size_t>(node.plane) >= planes_.size()) return false;
    visited[index] = 1;
    if (!visit(node.children[0], depth + 1) || !visit(node.children[1], depth + 1)) return false;
    for (const int child : node.children)
      height[index] = std::max(height[index], child < 0 ? 1 : height[child] + 1);
    visited[index] = 2;
    return depth + height[index] <= 512;
  };
  if (!visit(hull.root, 0)) return std::unexpected("Quake BSP collision tree is invalid");
  return {};
}

Contents BspWorld::At(const Vector& point) const {
  return CollisionHull(planes_, hulls_[0].nodes, hulls_[0].root).At(point);
}

Trace BspWorld::Sweep(const Vector& start, const Vector& end, const Vector& mins,
                      const Vector& maxs) const {
  const bool player = mins == kPlayerMins && maxs == kPlayerMaxs;
  assert(player || (mins == Vector{} && maxs == Vector{}));
  const auto& hull = hulls_[player ? 1 : 0];
  return CollisionHull(planes_, hull.nodes, hull.root)
      .Sweep(start, end, {},
             player ? CollisionHull::TraceKind::kPlayer : CollisionHull::TraceKind::kEntity);
}

}  // namespace modlock::quake
