#include "quake/collision_scene.h"

#include "quake/collision_hull.h"

namespace modlock::quake {

Trace CollisionScene::Sweep(const Vector& start, const Vector& end, const Vector& mins,
                            const Vector& maxs) const {
  auto closest = world_.Sweep(start, end, mins, maxs);
  const bool point = mins == Vector{} && maxs == Vector{};
  for (const auto& body : bodies_) {
    if (point && closest.all_solid) break;
    if (body.entity == ignored_entity_) continue;
    std::array<CollisionHull::Plane, 6> planes;
    std::array<CollisionHull::Node, 6> nodes;
    for (int index = 0; index < 6; ++index) {
      const int axis = index / 2;
      const int outside = index & 1;
      auto& plane = planes[index];
      plane.axis = axis;
      plane.normal[axis] = 1;
      plane.distance = outside ? body.mins[axis] - maxs[axis] : body.maxs[axis] - mins[axis];
      nodes[index].plane = index;
      nodes[index].children[outside] = -1;
      nodes[index].children[outside ^ 1] = index == 5 ? -2 : index + 1;
    }
    auto trace =
        CollisionHull(planes, nodes, 0)
            .Sweep(start, end, body.origin,
                   point ? CollisionHull::TraceKind::kEntity : CollisionHull::TraceKind::kPlayer);
    if (trace.fraction < closest.fraction || (point && (trace.all_solid || trace.start_solid))) {
      trace.entity = body.entity;
      if (point && closest.start_solid) trace.start_solid = true;
      closest = trace;
    }
  }
  return closest;
}

}  // namespace modlock::quake
