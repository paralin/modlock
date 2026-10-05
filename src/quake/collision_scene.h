#pragma once

#include <span>

#include "quake/collision_world.h"

namespace modlock::quake {

// CollisionBody is a solid entity snapshot in logical Quake coordinates.
// Entity zero belongs to the world; bodies have distinct positive identifiers.
struct CollisionBody {
  int entity;
  Vector origin{};
  Vector mins = kPlayerMins;
  Vector maxs = kPlayerMaxs;
};

// CollisionScene borrows one synchronous simulation snapshot. The world wins
// equal-time player contacts, followed by bodies in their supplied order. Point
// traces preserve server solid-start precedence. Ignoring the
// moving player or rocket owner changes neither identity nor contact ordering.
class CollisionScene final : public CollisionWorld {
 public:
  CollisionScene(const CollisionWorld& world, std::span<const CollisionBody> bodies,
                 int ignored_entity = -1)
      : world_(world), bodies_(bodies), ignored_entity_(ignored_entity) {}

  Trace Sweep(const Vector& start, const Vector& end, const Vector& mins,
              const Vector& maxs) const override;
  Contents At(const Vector& point) const override { return world_.At(point); }

 private:
  const CollisionWorld& world_;
  std::span<const CollisionBody> bodies_;
  int ignored_entity_;
};

}  // namespace modlock::quake
