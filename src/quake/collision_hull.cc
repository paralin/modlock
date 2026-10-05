#include "quake/collision_hull.h"

#include <algorithm>

namespace modlock::quake {
namespace {

Vector Between(const Vector& start, const Vector& end, float fraction) {
  Vector point{};
  for (std::size_t axis = 0; axis < point.size(); ++axis)
    point[axis] = start[axis] + fraction * (end[axis] - start[axis]);
  return point;
}

}  // namespace

float CollisionHull::Plane::Distance(const Vector& point) const {
  if (axis < 3) return point[axis] - distance;
  return normal[0] * point[0] + normal[1] * point[1] + normal[2] * point[2] - distance;
}

int CollisionHull::ContentsIn(int node, const Vector& point) const {
  while (node >= 0) {
    const auto& branch = nodes_[node];
    node = branch.children[planes_[branch.plane].Distance(point) < 0];
  }
  return node;
}

Contents CollisionHull::At(const Vector& point) const {
  return static_cast<Contents>(-ContentsIn(root_, point) - 1);
}

bool CollisionHull::TraceBranch(int node, float begin, float finish, const Vector& start,
                                const Vector& end, Trace& trace) const {
  if (node < 0) {
    if (node == -2)
      trace.start_solid = true;
    else
      trace.all_solid = false;
    return true;
  }
  const auto& branch = nodes_[node];
  const auto& plane = planes_[branch.plane];
  const float from = plane.Distance(start);
  const float to = plane.Distance(end);
  if (from >= 0 && to >= 0)
    return TraceBranch(branch.children[0], begin, finish, start, end, trace);
  if (from < 0 && to < 0) return TraceBranch(branch.children[1], begin, finish, start, end, trace);

  const int near = from < 0;
  float fraction =
      std::clamp(static_cast<float>((from + (near ? 0.03125 : -0.03125)) / (from - to)), 0.f, 1.f);
  float middle_time = begin + (finish - begin) * fraction;
  auto middle = Between(start, end, fraction);
  if (!TraceBranch(branch.children[near], begin, middle_time, start, middle, trace)) return false;
  if (ContentsIn(branch.children[near ^ 1], middle) != -2)
    return TraceBranch(branch.children[near ^ 1], middle_time, finish, middle, end, trace);
  if (trace.all_solid) return false;

  for (std::size_t axis = 0; axis < trace.normal.size(); ++axis)
    trace.normal[axis] = near ? -plane.normal[axis] : plane.normal[axis];
  while (ContentsIn(root_, middle) == -2) {
    fraction = static_cast<float>(fraction - 0.1);
    if (fraction < 0) break;
    middle_time = begin + (finish - begin) * fraction;
    middle = Between(start, end, fraction);
  }
  trace.fraction = middle_time;
  trace.end = middle;
  return false;
}

Trace CollisionHull::Sweep(const Vector& start, const Vector& end, const Vector& origin,
                           TraceKind kind) const {
  Vector local_start{}, local_end{};
  for (std::size_t axis = 0; axis < origin.size(); ++axis) {
    local_start[axis] = start[axis] - origin[axis];
    local_end[axis] = end[axis] - origin[axis];
  }
  Trace trace{.end = end, .all_solid = true};
  TraceBranch(root_, 0, 1, local_start, local_end, trace);
  if (kind == TraceKind::kPlayer) {
    if (trace.all_solid) trace.start_solid = true;
    if (trace.start_solid) trace.fraction = 0;
  }
  if (trace.fraction < 1) {
    for (std::size_t axis = 0; axis < origin.size(); ++axis) trace.end[axis] += origin[axis];
  }
  if (trace.fraction < 1 || trace.start_solid) trace.entity = 0;
  return trace;
}

}  // namespace modlock::quake
