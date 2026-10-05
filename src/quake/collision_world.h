#pragma once

#include <array>

namespace modlock::quake {

using Vector = std::array<float, 3>;

enum class Contents { kEmpty, kSolid, kWater, kSlime, kLava, kSky };

// Trace owns the result of a swept, axis-aligned box in logical Quake units.
struct Trace {
  Vector end{};
  Vector normal{};
  float fraction = 1;
  int entity = -1;
  bool start_solid = false;
  bool all_solid = false;
};

// CollisionWorld supplies the same world to authoritative and predicted movement.
// Queries are synchronous; the caller retains the world through the movement step.
// Standing-box traces use QuakeWorld player contact semantics. Point traces use
// server entity semantics: a solid start can report contact with fraction one.
class CollisionWorld {
 public:
  virtual ~CollisionWorld() = default;
  virtual Trace Sweep(const Vector& start, const Vector& end, const Vector& mins,
                      const Vector& maxs) const = 0;
  virtual Contents At(const Vector& point) const = 0;
};

inline constexpr Vector kPlayerMins{-16, -16, -24};
inline constexpr Vector kPlayerMaxs{16, 16, 32};

}  // namespace modlock::quake
