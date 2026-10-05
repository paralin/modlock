#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "quake/collision_hull.h"

namespace modlock::quake {

// BspWorld owns Quake v29 world-model collision. Hull zero traces points;
// hull one traces the standing Quake player. Authority and prediction consume
// identical compiled planes and liquid contents.
class BspWorld final : public CollisionWorld {
 public:
  // Decode owns the collision data or reports malformed, cyclic or unsupported
  // BSP input before any movement query can traverse it.
  static std::expected<BspWorld, std::string> Decode(std::span<const std::uint8_t> bytes);

  // Sweep accepts the standing Quake box or a zero-sized point. This world
  // contains only the static model; its consumer supplies dynamic entities.
  Trace Sweep(const Vector& start, const Vector& end, const Vector& mins,
              const Vector& maxs) const override;
  Contents At(const Vector& point) const override;

 private:
  BspWorld() = default;

  using Plane = CollisionHull::Plane;
  using Node = CollisionHull::Node;
  struct Hull {
    std::vector<Node> nodes;
    int root;
  };

  std::expected<void, std::string> Validate(const Hull& hull) const;

  std::vector<Plane> planes_;
  std::array<Hull, 2> hulls_;
};

}  // namespace modlock::quake
