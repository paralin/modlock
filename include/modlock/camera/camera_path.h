#pragma once

#include <expected>
#include <string>

#include "modlock/export.h"
#include "src/camera/camera.pb.h"

// Ported from deadlock-dolly (MIT, Copyright (c) 2026 Deadlock Dolly
// contributors): dolly/path.py evaluation contracts and
// native/include/dolly_flight.hpp input conventions.

namespace modlock::camera {

// kAspectRatioMin and kAspectRatioMax bound an authored framing ratio. Zero is
// the game's automatic-aspect sentinel and is never a curve value.
inline constexpr double kAspectRatioMin = 0.5;
inline constexpr double kAspectRatioMax = 4.0;
// kDefaultAspectRatio is the 16:9 framing ratio new paths start with.
inline constexpr double kDefaultAspectRatio = 16.0 / 9.0;
// kMaxKeys bounds one path's key count; the limit is an editor design choice,
// not a claim about the game.
inline constexpr std::size_t kMaxKeys = 100000;

// ValidatePath checks the path contract: finite key fields, aspect ratios
// within bounds, and strictly increasing nonnegative key times. Returns an
// error naming the offending key.
[[nodiscard]] MODLOCK_API std::expected<void, std::string> ValidatePath(const CameraPath& path);

// EvaluatePath returns the camera pose and aspect ratio at one shot-relative
// time without state: forward playback and rewinds are identical. Position
// uses nonuniform cubic Hermite; angles and aspect ratio use shape-preserving
// PCHIP so values cannot overshoot neighboring keys. Each channel holds its
// endpoint values outside its key range. Only yaw and roll wrap between
// authored keys in shortest mode; evaluated yaw and roll stay continuously
// unwrapped so crossing the 180-degree seam does not introduce a 360-degree
// jump. Degenerate inputs fall back to linear within the segment.
[[nodiscard]] MODLOCK_API std::expected<CameraPose, std::string> EvaluatePath(
    const CameraPath& path, double time);

// FlightInput is one rendered-frame movement sample in the engine's Z-up
// convention: yaw zero faces +X and positive pitch looks down.
struct FlightInput {
  double forward = 0;
  double right = 0;
  double up = 0;
  double pitch = 0;
  double yaw = 0;
  double roll = 0;
  double mouse_x = 0;
  double mouse_y = 0;
};

// IntegrateFlight advances pose from one input sample. A dt outside [0, 0.1]
// is discarded so suspension or Alt-Tab backlog cannot jump the camera; larger
// finite values clamp to 0.05. Speed is world units per second and sensitivity
// scales mouse deltas; invert_y flips mouse pitch.
MODLOCK_API void IntegrateFlight(CameraPose& pose, const FlightInput& input, double dt,
                                 double speed, double sensitivity, bool invert_y = false);

}  // namespace modlock::camera
