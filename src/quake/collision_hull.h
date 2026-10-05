#pragma once

#include <span>

#include "quake/collision_world.h"

namespace modlock::quake {

// CollisionHull borrows an acyclic Quake clip tree. Nonnegative children index
// nodes; negative children encode contents as -1 minus the Contents value.
// BspWorld validates loaded trees; CollisionScene constructs six-plane boxes.
class CollisionHull {
 public:
  enum class TraceKind { kPlayer, kEntity };

  struct Plane {
    Vector normal{};
    float distance = 0;
    int axis = 0;
    float Distance(const Vector& point) const;
  };
  struct Node {
    int plane;
    std::array<int, 2> children;
  };

  CollisionHull(std::span<const Plane> planes, std::span<const Node> nodes, int root)
      : planes_(planes), nodes_(nodes), root_(root) {}

  Contents At(const Vector& point) const;
  // Inputs and result use world coordinates; origin locates the local hull.
  // Player traces stop solid starts at fraction zero; entity traces retain the
  // exit endpoint and contact as Quake's server projectile movement requires.
  Trace Sweep(const Vector& start, const Vector& end, const Vector& origin = {},
              TraceKind kind = TraceKind::kPlayer) const;

 private:
  int ContentsIn(int node, const Vector& point) const;
  bool TraceBranch(int node, float begin, float finish, const Vector& start, const Vector& end,
                   Trace& trace) const;

  std::span<const Plane> planes_;
  std::span<const Node> nodes_;
  int root_;
};

}  // namespace modlock::quake
