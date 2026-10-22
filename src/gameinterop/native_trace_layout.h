#pragma once

#include <cstddef>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/native_trace.h"

namespace modlock::gameinterop {

// GameTrace is the installed VPhys2 contact layout shared by queries and
// projectile impact callbacks. Its entity pointers are borrowed on the engine thread.
struct alignas(16) GameTrace {
  void* surface = nullptr;
  void* entity = nullptr;
  void* hitbox = nullptr;
  void* body = nullptr;
  void* shape = nullptr;
  std::uint64_t contents = 0;
  std::byte transform[32]{};
  std::byte attributes[40]{};
  TraceVector start{};
  TraceVector end{};
  TraceVector normal{};
  TraceVector point{};
  float offset = 0;
  float fraction = 1;
  std::int32_t triangle = -1;
  // Copied with the triangle from the hit since game build 6711; -1 when unset.
  std::int32_t triangle_detail = -1;
  std::int16_t bone = -1;
  std::uint8_t ray_type = 0;
  bool start_solid = false;
  bool exact = false;
  std::byte padding[3]{};
};
static_assert(sizeof(GameTrace) == 192);
static_assert(offsetof(GameTrace, fraction) == 0xac);
static_assert(offsetof(GameTrace, start_solid) == 0xbb);

inline TraceResult ReadGameTrace(const GameTrace& native) {
  // Copy the trace's geometry.
  TraceResult result;
  result.position = native.end;
  result.normal = native.normal;
  result.start = native.start;
  result.end = native.end;
  result.exact_position = native.point;
  result.fraction = native.fraction;

  // Copy what it hit.
  result.hit_offset = native.offset;
  result.contents = native.contents;
  result.entity_handle = ReferenceHandleOf(native.entity);
  result.triangle = native.triangle;
  result.hitbox_bone = native.bone;
  result.start_in_solid = native.start_solid;
  result.exact_hit_point = native.exact;
  return result;
}

}  // namespace modlock::gameinterop
