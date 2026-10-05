#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "quake/collision_world.h"

namespace modlock::quake {

// MovementCommand contains one Quake input sample; angles are degrees and time is
// milliseconds. QuakeWorld sends whole milliseconds; a host whose tick is not a
// whole number of milliseconds passes its exact share so every side agrees.
struct MovementCommand {
  Vector angles{};
  float forward = 0;
  float side = 0;
  float up = 0;
  float milliseconds = 0;
  bool jump = false;
};

// MovementState contains the complete state restored before replaying a command.
struct MovementState {
  Vector origin{};
  Vector velocity{};
  float water_jump_seconds = 0;
  int ground_entity = -1;
  int water_level = 0;
  Contents water_type = Contents::kEmpty;
  bool jump_held = false;
  bool dead = false;
  bool spectator = false;
};

// MovementProfile fixes simulation constants in Quake units and seconds.
struct MovementProfile {
  float gravity = 800;
  float entity_gravity = 1;
  float max_speed = 320;
  float spectator_speed = 500;
  float acceleration = 10;
  float friction = 4;
  float stop_speed = 100;
  float water_acceleration = 10;
  float water_friction = 4;
};

// MovementResult retains ordered contacts for the server's trigger/touch dispatch.
struct MovementResult {
  MovementState state;
  // A byte-length command subdivides at most eight ways. Each step can touch
  // two ground probes and four planes on each of two slide paths.
  std::array<int, 80> contacts{};
  std::size_t contact_count = 0;
};

// Move applies one command, including the original subdivision of commands above 50 ms.
// The collision world and profile are identical for prediction and authority.
MovementResult Move(const MovementState& state, const MovementCommand& command,
                    const CollisionWorld& world, const MovementProfile& profile = {});

}  // namespace modlock::quake
