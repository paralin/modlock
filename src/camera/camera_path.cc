#include "modlock/camera/camera_path.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

// Ported from deadlock-dolly (MIT, Copyright (c) 2026 Deadlock Dolly
// contributors): dolly/path.py interpolation algorithms, preserved exactly
// including degenerate-case fallbacks.

namespace modlock::camera {
namespace {

// kPi converts degrees to radians.
constexpr double kPi = 3.14159265358979323846;

// Finite reports whether value is a usable number.
bool Finite(double value) noexcept {
  return std::isfinite(value);
}

// ChannelValues extracts one channel from every key in path order.
std::vector<double> ChannelValues(const modlock::CameraPath& path, int channel) {
  std::vector<double> values;
  values.reserve(static_cast<std::size_t>(path.keys_size()));
  for (const auto& key : path.keys()) {
    switch (channel) {
      case 0:
        values.push_back(key.position().x());
        break;
      case 1:
        values.push_back(key.position().y());
        break;
      case 2:
        values.push_back(key.position().z());
        break;
      case 3:
        values.push_back(key.angles().pitch());
        break;
      case 4:
        values.push_back(key.angles().yaw());
        break;
      case 5:
        values.push_back(key.angles().roll());
        break;
      default:
        values.push_back(key.aspect_ratio());
        break;
    }
  }
  return values;
}

// WrapAngle reduces degrees to the (-180, 180] range. fmod keeps the
// dividend's sign, so normalize into [0, 360) before subtracting.
double WrapAngle(double value) noexcept {
  double shifted = std::fmod(value + 180.0, 360.0);
  if (shifted < 0) {
    shifted += 360.0;
  }
  return shifted - 180.0;
}

// UnwrapAngles reduces each angle then accumulates shortest-path deltas. An
// exactly half-turn delta follows the sign the user explicitly authored.
std::vector<double> UnwrapAngles(const std::vector<double>& authored) {
  // Wrap each angle.
  if (authored.empty()) {
    return {};
  }
  std::vector<double> wrapped;
  wrapped.reserve(authored.size());
  for (double value : authored) {
    wrapped.push_back(WrapAngle(value));
  }

  // Take the short way from each angle to the next; a half turn follows the authored direction.
  std::vector<double> result;
  result.reserve(authored.size());
  result.push_back(wrapped.front());
  for (std::size_t i = 1; i < wrapped.size(); ++i) {
    double delta = WrapAngle(wrapped[i] - result[i - 1]);
    if (delta == -180.0 && authored[i] > authored[i - 1]) {
      delta = 180.0;
    }
    result.push_back(result[i - 1] + delta);
  }
  return result;
}

// PositionTangents computes secant-based Hermite derivatives for nonuniform
// times. Central secants weight by the actual time intervals so a tiny
// neighboring interval cannot amplify a long-segment tangent.
std::vector<double> PositionTangents(const std::vector<double>& times,
                                     const std::vector<double>& values) {
  // Measure each segment's slope; two keys share one.
  std::vector<double> slopes;
  slopes.reserve(times.size() - 1);
  for (std::size_t i = 0; i + 1 < times.size(); ++i) {
    slopes.push_back((values[i + 1] - values[i]) / (times[i + 1] - times[i]));
  }
  if (times.size() == 2) {
    return {slopes[0], slopes[0]};
  }

  // Inner keys take the slope across their neighbors; the ends take their segment's.
  std::vector<double> derivatives;
  derivatives.reserve(times.size());
  derivatives.push_back(slopes[0]);
  for (std::size_t i = 1; i + 1 < times.size(); ++i) {
    derivatives.push_back((values[i + 1] - values[i - 1]) / (times[i + 1] - times[i - 1]));
  }
  derivatives.push_back(slopes.back());
  return derivatives;
}

// MonotoneTangents computes Fritsch-Butland/PCHIP derivatives for nonuniform
// sample times so the curve cannot overshoot neighboring keys.
std::vector<double> MonotoneTangents(const std::vector<double>& times,
                                     const std::vector<double>& values) {
  // Measure each segment's spacing and slope; two keys share one.
  const std::size_t n = times.size();
  std::vector<double> spacing(n - 1);
  std::vector<double> slopes(n - 1);
  for (std::size_t i = 0; i + 1 < n; ++i) {
    spacing[i] = times[i + 1] - times[i];
    slopes[i] = (values[i + 1] - values[i]) / spacing[i];
  }
  if (n == 2) {
    return {slopes[0], slopes[0]};
  }

  // Inner tangents take the weighted harmonic mean, zero at an extremum.
  std::vector<double> derivatives(n, 0.0);
  for (std::size_t i = 1; i + 1 < n; ++i) {
    double left = slopes[i - 1], right = slopes[i];
    if (left == 0 || right == 0 || (left > 0) != (right > 0)) {
      continue;
    }
    double w1 = 2 * spacing[i] + spacing[i - 1];
    double w2 = spacing[i] + 2 * spacing[i - 1];
    derivatives[i] = (w1 + w2) / (w1 / left + w2 / right);
  }

  // The ends take the three point slope, clamped to keep the curve monotone.
  auto endpoint = [](double here_h, double next_h, double here_s, double next_s) {
    double derivative = ((2 * here_h + next_h) * here_s - here_h * next_s) / (here_h + next_h);
    if (here_s == 0 || (derivative > 0) != (here_s > 0)) {
      return 0.0;
    }
    if ((here_s > 0) != (next_s > 0) && std::abs(derivative) > std::abs(3 * here_s)) {
      return 3 * here_s;
    }
    return derivative;
  };
  derivatives[0] = endpoint(spacing[0], spacing[1], slopes[0], slopes[1]);
  derivatives[n - 1] = endpoint(spacing[n - 2], spacing[n - 3], slopes[n - 2], slopes[n - 3]);
  return derivatives;
}

// SampleChannel evaluates one channel at time. Monotone selects PCHIP; the
// position channels use unbounded Hermite tangents instead.
double SampleChannel(const std::vector<double>& times, const std::vector<double>& values,
                     double time, modlock::Interpolation interpolation, bool monotone) {
  // Before the first key and after the last, hold the end value.
  if (time <= times.front() || times.size() == 1) {
    return values.front();
  }
  if (time >= times.back()) {
    return values.back();
  }

  // Left is the last key at or before time; times are strictly increasing.
  std::size_t left =
      static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), time) - times.begin()) -
      1;
  if (interpolation == modlock::INTERPOLATION_STEP) {
    return values[left];
  }
  double span = times[left + 1] - times[left];
  double fraction = (time - times[left]) / span;
  if (interpolation == modlock::INTERPOLATION_LINEAR || times.size() == 2) {
    return (1 - fraction) * values[left] + fraction * values[left + 1];
  }

  // Division by zero yields inf rather than a trap: extremely close
  // timestamps can exceed floating-point derivative precision, and a linear
  // segment remains finite and honors both keys.
  const std::vector<double> tangents =
      monotone ? MonotoneTangents(times, values) : PositionTangents(times, values);
  bool finite = true;
  for (double t : tangents) {
    if (!Finite(t)) {
      finite = false;
      break;
    }
  }
  if (!finite) {
    return (1 - fraction) * values[left] + fraction * values[left + 1];
  }

  // Evaluate the Hermite segment, falling back to linear on overflow.
  const double u2 = fraction * fraction;
  const double u3 = u2 * fraction;
  double value = (2 * u3 - 3 * u2 + 1) * values[left] +
                 (u3 - 2 * u2 + fraction) * span * tangents[left] +
                 (-2 * u3 + 3 * u2) * values[left + 1] + (u3 - u2) * span * tangents[left + 1];
  if (!Finite(value)) {
    return (1 - fraction) * values[left] + fraction * values[left + 1];
  }
  if (monotone) {
    // Also contain floating-point rounding at segment boundaries.
    double low = std::min(values[left], values[left + 1]);
    double high = std::max(values[left], values[left + 1]);
    value = std::max(low, std::min(high, value));
  }
  return value;
}

}  // namespace

std::expected<void, std::string> ValidatePath(const modlock::CameraPath& path) {
  const double previous_time = -std::numeric_limits<double>::infinity();
  double last_time = previous_time;
  for (int i = 0; i < path.keys_size(); ++i) {
    const auto& key = path.keys(i);
    const double time = key.time();
    if (!Finite(time) || time < 0) {
      return std::unexpected("camera key " + std::to_string(i) +
                             " time must be a nonnegative finite number");
    }
    if (time <= last_time) {
      return std::unexpected(
          "camera key times must be strictly increasing; duplicates are not allowed");
    }
    last_time = time;
    if (!Finite(key.position().x()) || !Finite(key.position().y()) || !Finite(key.position().z())) {
      return std::unexpected("camera key " + std::to_string(i) + " position must be finite");
    }
    if (!Finite(key.angles().pitch()) || !Finite(key.angles().yaw()) ||
        !Finite(key.angles().roll())) {
      return std::unexpected("camera key " + std::to_string(i) + " angles must be finite");
    }
    if (!Finite(key.aspect_ratio()) || key.aspect_ratio() < kAspectRatioMin ||
        key.aspect_ratio() > kAspectRatioMax) {
      return std::unexpected("camera key " + std::to_string(i) +
                             " aspect_ratio must be between 0.5 and 4");
    }
  }
  return {};
}

std::expected<modlock::CameraPose, std::string> EvaluatePath(const modlock::CameraPath& path,
                                                             double time) {
  // Refuse an empty or invalid path, or a nonfinite time.
  if (path.keys_size() == 0) {
    return std::unexpected("add a camera keyframe before evaluating the path");
  }
  if (!Finite(time)) {
    return std::unexpected("evaluation time must be finite");
  }
  if (auto validated = ValidatePath(path); !validated) {
    return std::unexpected(validated.error());
  }

  // Collect the key times.
  std::vector<double> times;
  times.reserve(static_cast<std::size_t>(path.keys_size()));
  for (const auto& key : path.keys()) {
    times.push_back(key.time());
  }

  // Unspecified matches the authored default: shortest-path rotation.
  const bool shortest = path.rotation_mode() != modlock::ROTATION_MODE_UNWRAPPED;
  struct Channel {
    int field;
    bool monotone;
    bool wrap;
    modlock::Interpolation interpolation;
  };
  const std::array<Channel, 7> channels{{
      {0, false, false, path.interpolation()},      // x
      {1, false, false, path.interpolation()},      // y
      {2, false, false, path.interpolation()},      // z
      {3, true, false, path.interpolation()},       // pitch
      {4, true, shortest, path.interpolation()},    // yaw
      {5, true, shortest, path.interpolation()},    // roll
      {6, true, false, path.lens_interpolation()},  // aspect ratio
  }};

  // Sample each channel into the pose.
  modlock::CameraPose result;
  for (const auto& channel : channels) {
    std::vector<double> values = ChannelValues(path, channel.field);
    if (channel.wrap) {
      values = UnwrapAngles(values);
    }
    const double value =
        SampleChannel(times, values, time, channel.interpolation, channel.monotone);
    switch (channel.field) {
      case 0:
        result.mutable_position()->set_x(value);
        break;
      case 1:
        result.mutable_position()->set_y(value);
        break;
      case 2:
        result.mutable_position()->set_z(value);
        break;
      case 3:
        result.mutable_angles()->set_pitch(value);
        break;
      case 4:
        result.mutable_angles()->set_yaw(value);
        break;
      case 5:
        result.mutable_angles()->set_roll(value);
        break;
      default:
        result.set_aspect_ratio(value);
        break;
    }
  }
  return result;
}

void IntegrateFlight(modlock::CameraPose& pose, const FlightInput& input, double dt, double speed,
                     double sensitivity, bool invert_y) {
  // Discard a backlog step and cap the rest.
  if (!Finite(dt) || dt < 0 || dt > .1) {
    return;  // discard suspension/Alt-Tab backlog
  }
  dt = std::min(dt, .05);
  const double rad = kPi / 180;

  // Pitch by the keys and the mouse, short of straight up or down.
  pose.mutable_angles()->set_pitch(std::clamp(pose.angles().pitch() + input.pitch * 75 * dt +
                                                  input.mouse_y * sensitivity * (invert_y ? -1 : 1),
                                              -89.9, 89.9));

  // std::remainder gives the result in [-180, 180] with the correct sign for
  // negatives, matching Dolly's use of std::remainder for flight angles.
  pose.mutable_angles()->set_yaw(std::remainder(
      pose.angles().yaw() + input.yaw * 75 * dt - input.mouse_x * sensitivity, 360.0));
  pose.mutable_angles()->set_roll(
      std::remainder(pose.angles().roll() + input.roll * 60 * dt, 360.0));

  // Move along the view, no faster on a diagonal.
  const double pitch = pose.angles().pitch() * rad;
  const double yaw = pose.angles().yaw() * rad;
  double x = std::cos(pitch) * std::cos(yaw) * input.forward + std::sin(yaw) * input.right;
  double y = std::cos(pitch) * std::sin(yaw) * input.forward - std::cos(yaw) * input.right;
  double z = -std::sin(pitch) * input.forward + input.up;
  const double norm = std::sqrt(x * x + y * y + z * z);
  if (norm > 1) {
    x /= norm;
    y /= norm;
    z /= norm;
  }

  // Step the position.
  pose.mutable_position()->set_x(pose.position().x() + x * speed * dt);
  pose.mutable_position()->set_y(pose.position().y() + y * speed * dt);
  pose.mutable_position()->set_z(pose.position().z() + z * speed * dt);
}

}  // namespace modlock::camera
