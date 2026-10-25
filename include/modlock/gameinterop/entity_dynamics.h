#pragma once

#include <expected>
#include <string>

#include "modlock/export.h"

namespace modlock::gameinterop {

class ModuleImage;

// EntityDynamics holds the engine's velocity calls on CBaseEntity. For an
// entity the physics simulates, MOVETYPE_VPHYSICS such as a prop_physics, they
// read and change its physics body, so the simulation carries the motion on
// against the world and other bodies. For any other entity they use its
// m_vecAbsVelocity and m_vecAngVelocity fields. Each borrows a live entity on
// the server frame thread.
struct MODLOCK_API EntityDynamics {
  // velocity writes the entity's velocity, in units per second, to its second
  // argument and returns it.
  float* (*velocity)(void* entity, float* out);
  // push adds a velocity, in units per second, in world axes.
  void (*push)(void* entity, const float* velocity);
  // spin adds an angular velocity, in degrees per second about the entity's own
  // axes.
  void (*spin)(void* entity, const float* angular_velocity);

  // Resolve finds the three calls in server.dll.
  static std::expected<EntityDynamics, std::string> Resolve(const ModuleImage& server);
};

}  // namespace modlock::gameinterop
