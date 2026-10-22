#include "modlock/gameinterop/teleport_pawn_control.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "modlock/gameinterop/entity_abi.h"
#include "proto/modlock/types.pb.h"

namespace modlock::gameinterop {
namespace {

bool Finite(const std::array<float, 3>& vector) {
  return std::all_of(vector.begin(), vector.end(),
                     [](float value) { return std::isfinite(value); });
}

std::array<float, 3> Position(const Vec3& position) {
  return {static_cast<float>(position.x()), static_cast<float>(position.y()),
          static_cast<float>(position.z())};
}

}  // namespace

bool TeleportPawnControl::TeleportToPosition(int32_t slot, const Vec3& position) {
  // Teleport a live pawn to a finite position.
  const auto target = Position(position);
  if (!Finite(target)) {
    return false;
  }
  void* pawn = observer_.PawnForSlot(slot);
  if (!pawn) {
    return false;
  }
  TeleportEntity(pawn, target[0], target[1], target[2]);
  return true;
}

bool TeleportPawnControl::ResetToPose(int32_t slot, const Vec3& position,
                                      const EulerAngles& angles) {
  // Reset a live pawn's camera to a finite pose.
  if (!reset_camera_) {
    return false;
  }
  const auto target = Position(position);
  const std::array<float, 3> view{static_cast<float>(angles.pitch()),
                                  static_cast<float>(angles.yaw()),
                                  static_cast<float>(angles.roll())};
  if (!Finite(target) || !Finite(view)) {
    return false;
  }
  void* pawn = observer_.PawnForSlot(slot);
  if (!pawn) {
    return false;
  }
  reset_camera_(nullptr, pawn, target.data(), view.data());
  return true;
}

}  // namespace modlock::gameinterop
