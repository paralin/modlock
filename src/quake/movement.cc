#include "quake/movement.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace modlock::quake {
namespace {

float Dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vector AddScaled(Vector a, const Vector& b, float scale) {
  for (std::size_t axis = 0; axis < a.size(); ++axis) a[axis] += b[axis] * scale;
  return a;
}

float Normalize(Vector& value) {
  const float length = std::sqrt(Dot(value, value));
  if (length != 0) {
    const float inverse = 1.f / length;
    for (auto& component : value) component *= inverse;
  }
  return length;
}

Vector Clip(const Vector& velocity, const Vector& normal) {
  Vector result = AddScaled(velocity, normal, -Dot(velocity, normal));
  for (auto& component : result) {
    if (component > -0.1f && component < 0.1f) component = 0;
  }
  return result;
}

bool Liquid(Contents contents) { return contents >= Contents::kWater; }

// Step borrows its world and profile for one synchronous command, including subdivisions.
class Step {
 public:
  Step(const MovementState& state, const CollisionWorld& world, const MovementProfile& profile)
      : result_{.state = state}, state_(result_.state), world_(world), profile_(profile) {}

  MovementResult Run(const MovementCommand& command) {
    if (command.milliseconds > 50) {
      auto half = command;
      half.milliseconds = std::floor(command.milliseconds / 2);
      result_ = Step(state_, world_, profile_).Run(half);
      const auto second = Step(state_, world_, profile_).Run(half);
      state_ = second.state;
      for (std::size_t i = 0; i < second.contact_count; ++i) Touch(second.contacts[i]);
      return result_;
    }

    dt_ = static_cast<float>(command.milliseconds * 0.001);
    Axes(command.angles);
    if (state_.spectator) {
      Spectator(command);
      return result_;
    }

    Nudge();
    Categorize();
    if (state_.water_level == 2) WaterJump();
    if (state_.velocity[2] < 0) state_.water_jump_seconds = 0;
    if (command.jump)
      Jump();
    else
      state_.jump_held = false;
    Friction();
    if (state_.water_level >= 2)
      Swim(command);
    else
      WalkOrFly(command);
    Categorize();
    return result_;
  }

 private:
  void Axes(const Vector& angles) {
    const auto radians = [](float degrees) {
      return static_cast<float>(degrees * (std::numbers::pi * 2 / 360));
    };
    const float sy = std::sin(static_cast<double>(radians(angles[1])));
    const float cy = std::cos(static_cast<double>(radians(angles[1])));
    const float sp = std::sin(static_cast<double>(radians(angles[0])));
    const float cp = std::cos(static_cast<double>(radians(angles[0])));
    const float sr = std::sin(static_cast<double>(radians(angles[2])));
    const float cr = std::cos(static_cast<double>(radians(angles[2])));
    forward_ = {cp * cy, cp * sy, -sp};
    right_ = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
  }

  Trace Sweep(const Vector& start, const Vector& end) const {
    return world_.Sweep(start, end, kPlayerMins, kPlayerMaxs);
  }

  void Touch(int entity) { result_.contacts[result_.contact_count++] = entity; }

  void Nudge() {
    constexpr std::array<float, 3> shifts{0, -0.125f, 0.125f};
    const auto origin = state_.origin;
    for (float z : shifts) {
      for (float x : shifts) {
        for (float y : shifts) {
          const Vector candidate{origin[0] + x, origin[1] + y, origin[2] + z};
          const auto trace = Sweep(candidate, candidate);
          if (!trace.start_solid && !trace.all_solid) {
            state_.origin = candidate;
            return;
          }
        }
      }
    }
  }

  void Categorize() {
    auto below = state_.origin;
    below[2] -= 1;
    state_.ground_entity = -1;
    if (state_.velocity[2] <= 180) {
      const auto trace = Sweep(state_.origin, below);
      if (trace.normal[2] >= 0.7f) state_.ground_entity = trace.entity;
      if (state_.ground_entity != -1) {
        state_.water_jump_seconds = 0;
        if (!trace.start_solid && !trace.all_solid) state_.origin = trace.end;
      }
      if (trace.entity > 0) Touch(trace.entity);
    }

    state_.water_level = 0;
    state_.water_type = Contents::kEmpty;
    auto point = state_.origin;
    point[2] += kPlayerMins[2] + 1;
    const auto contents = world_.At(point);
    if (!Liquid(contents)) return;
    state_.water_type = contents;
    state_.water_level = 1;
    point[2] = state_.origin[2] + (kPlayerMins[2] + kPlayerMaxs[2]) * 0.5f;
    if (!Liquid(world_.At(point))) return;
    state_.water_level = 2;
    point[2] = state_.origin[2] + 22;
    if (Liquid(world_.At(point))) state_.water_level = 3;
  }

  void WaterJump() {
    if (state_.water_jump_seconds != 0 || state_.velocity[2] < -180) return;
    Vector forward{forward_[0], forward_[1], 0};
    Normalize(forward);
    auto point = AddScaled(state_.origin, forward, 24);
    point[2] += 8;
    if (world_.At(point) != Contents::kSolid) return;
    point[2] += 24;
    if (world_.At(point) != Contents::kEmpty) return;
    state_.velocity = AddScaled({}, forward, 50);
    state_.velocity[2] = 310;
    state_.water_jump_seconds = 2;
    state_.jump_held = true;
  }

  void Jump() {
    if (state_.dead) {
      state_.jump_held = true;
      return;
    }
    if (state_.water_jump_seconds != 0) {
      state_.water_jump_seconds = std::max(0.f, state_.water_jump_seconds - dt_);
      return;
    }
    if (state_.water_level >= 2) {
      state_.ground_entity = -1;
      state_.velocity[2] = state_.water_type == Contents::kWater   ? 100.f
                           : state_.water_type == Contents::kSlime ? 80.f
                                                                   : 50.f;
      return;
    }
    if (state_.ground_entity == -1 || state_.jump_held) return;
    state_.ground_entity = -1;
    state_.velocity[2] += 270;
    state_.jump_held = true;
  }

  void Friction() {
    if (state_.water_jump_seconds != 0) return;
    const float speed = std::sqrt(Dot(state_.velocity, state_.velocity));
    if (speed < 1) {
      state_.velocity[0] = state_.velocity[1] = 0;
      return;
    }
    float friction = profile_.friction;
    if (state_.ground_entity != -1) {
      Vector start{state_.origin[0] + state_.velocity[0] / speed * 16,
                   state_.origin[1] + state_.velocity[1] / speed * 16,
                   state_.origin[2] + kPlayerMins[2]};
      auto end = start;
      end[2] -= 34;
      if (Sweep(start, end).fraction == 1) friction *= 2;
    }
    float drop = 0;
    if (state_.water_level >= 2)
      drop = speed * profile_.water_friction * state_.water_level * dt_;
    else if (state_.ground_entity != -1)
      drop = std::max(speed, profile_.stop_speed) * friction * dt_;
    const float scale = std::max(0.f, speed - drop) / speed;
    for (auto& component : state_.velocity) component *= scale;
  }

  void Accelerate(const Vector& direction, float speed, float acceleration, bool air) {
    if (state_.dead || state_.water_jump_seconds != 0) return;
    const float projection = air ? std::min(speed, 30.f) : speed;
    const float remaining = projection - Dot(state_.velocity, direction);
    if (remaining <= 0) return;
    const float amount =
        std::min(remaining, air ? acceleration * speed * dt_ : acceleration * dt_ * speed);
    state_.velocity = AddScaled(state_.velocity, direction, amount);
  }

  void Slide() {
    const auto initial_velocity = state_.velocity;
    const auto original_velocity = state_.velocity;
    std::array<Vector, 5> planes{};
    std::size_t count = 0;
    float remaining = dt_;
    for (int bump = 0; bump < 4; ++bump) {
      const auto trace = Sweep(state_.origin, AddScaled(state_.origin, state_.velocity, remaining));
      if (trace.start_solid || trace.all_solid) {
        state_.velocity = {};
        return;
      }
      if (trace.fraction > 0) {
        state_.origin = trace.end;
        count = 0;
      }
      if (trace.fraction == 1) break;
      Touch(trace.entity);
      remaining -= remaining * trace.fraction;
      if (count == planes.size()) {
        state_.velocity = {};
        break;
      }
      planes[count++] = trace.normal;
      bool accepted = false;
      for (std::size_t i = 0; i < count; ++i) {
        state_.velocity = Clip(original_velocity, planes[i]);
        accepted = true;
        for (std::size_t j = 0; j < count; ++j) {
          if (i != j && Dot(state_.velocity, planes[j]) < 0) {
            accepted = false;
            break;
          }
        }
        if (accepted) break;
      }
      if (!accepted) {
        if (count != 2) {
          state_.velocity = {};
          break;
        }
        const Vector crease{planes[0][1] * planes[1][2] - planes[0][2] * planes[1][1],
                            planes[0][2] * planes[1][0] - planes[0][0] * planes[1][2],
                            planes[0][0] * planes[1][1] - planes[0][1] * planes[1][0]};
        state_.velocity = AddScaled({}, crease, Dot(crease, state_.velocity));
      }
      if (Dot(state_.velocity, initial_velocity) <= 0) {
        state_.velocity = {};
        break;
      }
    }
    if (state_.water_jump_seconds != 0) state_.velocity = initial_velocity;
  }

  void Ground() {
    state_.velocity[2] = 0;
    if (state_.velocity[0] == 0 && state_.velocity[1] == 0) return;
    const auto direct = Sweep(state_.origin, AddScaled(state_.origin, state_.velocity, dt_));
    if (direct.fraction == 1) {
      state_.origin = direct.end;
      return;
    }
    const auto start = state_.origin;
    const auto velocity = state_.velocity;
    Slide();
    const auto lower = state_;
    state_.origin = start;
    state_.velocity = velocity;
    auto end = start;
    end[2] += 18;
    const auto up = Sweep(start, end);
    if (!up.start_solid && !up.all_solid) state_.origin = up.end;
    Slide();
    end = state_.origin;
    end[2] -= 18;
    const auto down = Sweep(state_.origin, end);
    if (down.normal[2] < 0.7f) {
      state_ = lower;
      return;
    }
    if (!down.start_solid && !down.all_solid) state_.origin = down.end;
    const auto distance = [&start](const Vector& position) {
      const float x = position[0] - start[0];
      const float y = position[1] - start[1];
      return x * x + y * y;
    };
    if (distance(lower.origin) > distance(state_.origin))
      state_ = lower;
    else
      state_.velocity[2] = lower.velocity[2];
  }

  void WalkOrFly(const MovementCommand& command) {
    forward_[2] = right_[2] = 0;
    Normalize(forward_);
    Normalize(right_);
    auto wish = AddScaled(AddScaled({}, forward_, command.forward), right_, command.side);
    wish[2] = 0;
    const float speed = std::min(Normalize(wish), profile_.max_speed);
    const bool grounded = state_.ground_entity != -1;
    if (grounded) state_.velocity[2] = 0;
    Accelerate(wish, speed, profile_.acceleration, !grounded);
    state_.velocity[2] -= profile_.entity_gravity * profile_.gravity * dt_;
    if (grounded)
      Ground();
    else
      Slide();
  }

  void Swim(const MovementCommand& command) {
    auto wish = AddScaled(AddScaled({}, forward_, command.forward), right_, command.side);
    if (command.forward == 0 && command.side == 0 && command.up == 0)
      wish[2] -= 60;
    else
      wish[2] += command.up;
    const float speed = std::min(Normalize(wish), profile_.max_speed) * 0.7f;
    Accelerate(wish, speed, profile_.water_acceleration, false);
    const auto destination = AddScaled(state_.origin, state_.velocity, dt_);
    auto above = destination;
    above[2] += 19;
    const auto trace = Sweep(above, destination);
    if (!trace.start_solid && !trace.all_solid)
      state_.origin = trace.end;
    else
      Slide();
  }

  void Spectator(const MovementCommand& command) {
    const float speed = std::sqrt(Dot(state_.velocity, state_.velocity));
    if (speed < 1)
      state_.velocity = {};
    else {
      const float drop = std::max(speed, profile_.stop_speed) * profile_.friction * 1.5f * dt_;
      const float scale = std::max(0.f, speed - drop) / speed;
      for (auto& component : state_.velocity) component *= scale;
    }
    Normalize(forward_);
    Normalize(right_);
    auto wish = AddScaled(AddScaled({}, forward_, command.forward), right_, command.side);
    wish[2] += command.up;
    const float wish_speed = std::min(Normalize(wish), profile_.spectator_speed);
    const float remaining = wish_speed - Dot(state_.velocity, wish);
    if (remaining <= 0) return;
    state_.velocity = AddScaled(state_.velocity, wish,
                                std::min(remaining, profile_.acceleration * dt_ * wish_speed));
    state_.origin = AddScaled(state_.origin, state_.velocity, dt_);
  }

  MovementResult result_;
  MovementState& state_;
  const CollisionWorld& world_;
  const MovementProfile& profile_;
  float dt_ = 0;
  Vector forward_{};
  Vector right_{};
};

}  // namespace

MovementResult Move(const MovementState& state, const MovementCommand& command,
                    const CollisionWorld& world, const MovementProfile& profile) {
  return Step(state, world, profile).Run(command);
}

}  // namespace modlock::quake
