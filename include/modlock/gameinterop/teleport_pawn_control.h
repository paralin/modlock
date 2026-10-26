#pragma once

#include "modlock/export.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "modlock/plugin.h"

namespace modlock::gameinterop {

// TeleportPawnControl adapts the observer's located pawns to the
// plugin PawnControl seam: TeleportToPosition resolves the slot's observed
// pawn and issues the raw CBaseEntity::Teleport vcall.
// ResetToPose uses the supplied native helper to reset velocity and client view.
//
// Slots without a live pawn report failure without
// touching anything.
class MODLOCK_API TeleportPawnControl final : public modlock::PawnControl {
 public:
  explicit TeleportPawnControl(const PawnObserver& observer,
                               TeleportClientCamera reset_camera = nullptr)
      : observer_(observer), reset_camera_(reset_camera) {}

  bool TeleportToPosition(int32_t slot, const geometry::Vec3& position) override;

  bool ResetToPose(int32_t slot, const geometry::Vec3& position,
                   const geometry::EulerAngles& angles) override;

 private:
  const PawnObserver& observer_;
  TeleportClientCamera reset_camera_;
};

}  // namespace modlock::gameinterop
