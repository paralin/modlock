#include "modlock/camera/camera_path.h"

#include <cmath>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace modlock::camera {
namespace {

// Key makes one keyframe with defaults matching the Dolly test helpers.
modlock::CameraKeyframe Key(double time, double x = 0, double y = 0, double z = 0, double pitch = 0,
                            double yaw = 0, double roll = 0, double aspect = kDefaultAspectRatio) {
  // Set the time and position.
  modlock::CameraKeyframe key;
  key.set_time(time);
  key.mutable_position()->set_x(x);
  key.mutable_position()->set_y(y);
  key.mutable_position()->set_z(z);

  // Set the angles and lens.
  key.mutable_angles()->set_pitch(pitch);
  key.mutable_angles()->set_yaw(yaw);
  key.mutable_angles()->set_roll(roll);
  key.set_aspect_ratio(aspect);
  return key;
}

// Path makes a default-interpolation path from keys.
modlock::CameraPath MakePath(std::initializer_list<modlock::CameraKeyframe> keys,
                             modlock::Interpolation interpolation = modlock::INTERPOLATION_SMOOTH,
                             modlock::RotationMode rotation = modlock::ROTATION_MODE_SHORTEST,
                             modlock::Interpolation lens = modlock::INTERPOLATION_SMOOTH) {
  // Set the modes and append each key.
  modlock::CameraPath path;
  path.set_interpolation(interpolation);
  path.set_rotation_mode(rotation);
  path.set_lens_interpolation(lens);
  for (const auto& key : keys) {
    *path.add_keys() = key;
  }
  return path;
}

// ExpectClose checks value against expected within tolerance.
void ExpectClose(double actual, double expected, double tolerance = 1e-9) {
  EXPECT_NEAR(actual, expected, tolerance);
}

TEST(CameraPathTest, EmptyPathValidatesButCannotEvaluate) {
  // An empty path is valid, but has no pose.
  modlock::CameraPath path;
  EXPECT_TRUE(ValidatePath(path).has_value());
  auto evaluated = EvaluatePath(path, 0);
  EXPECT_FALSE(evaluated.has_value());
  EXPECT_NE(evaluated.error().find("camera keyframe"), std::string::npos);
}

TEST(CameraPathTest, SingleKeyHoldsAndNegativeEvaluationIsClamped) {
  auto path = MakePath({Key(2, 5, 0, 0, 0, 720)});
  for (double time : {-10.0, 0.0, 2.0, 900.0}) {
    auto evaluated = EvaluatePath(path, time);
    ASSERT_TRUE(evaluated.has_value());
    ExpectClose(evaluated->position().x(), 5);
    // 720 wraps to 0 in shortest mode.
    ExpectClose(evaluated->angles().yaw(), 0);
  }
}

TEST(CameraPathTest, LinearMovementAndIndependentLens) {
  // Aspect ratio interpolates on its own channel with linear movement here.
  auto path = MakePath({Key(1, 10, -20, 0, 0, 0, 0, 1.2), Key(3, 30, 20, 0, 0, 0, 0, 2.4)},
                       modlock::INTERPOLATION_LINEAR);
  auto evaluated = EvaluatePath(path, 1.5);
  ASSERT_TRUE(evaluated.has_value());
  ExpectClose(evaluated->position().x(), 15);
  ExpectClose(evaluated->position().y(), -10);
  ExpectClose(evaluated->aspect_ratio(), 1.5);
}

TEST(CameraPathTest, UnevenSmoothPathReproducesConstantVelocity) {
  modlock::CameraPath path;
  for (double t : {0.0, 0.1, 2.3, 10.0}) {
    *path.add_keys() = Key(t, 3 * t + 1, -2 * t, 0.5 * t);
  }
  for (double time : {0.0, 0.04, 0.1, 0.8, 2.3, 7.1, 10.0}) {
    auto evaluated = EvaluatePath(path, time);
    ASSERT_TRUE(evaluated.has_value());
    ExpectClose(evaluated->position().x(), 3 * time + 1, 1e-7);
    ExpectClose(evaluated->position().y(), -2 * time, 1e-7);
    ExpectClose(evaluated->position().z(), 0.5 * time, 1e-7);
  }
}

TEST(CameraPathTest, SmoothPositionsAreContinuouslyDifferentiableAtKeys) {
  // Sample the path just either side of the middle key.
  auto path = MakePath({Key(0, 0), Key(0.4, 5), Key(4, -10)});
  const double eps = 1e-6;
  auto center = EvaluatePath(path, 0.4);
  auto left = EvaluatePath(path, 0.4 - eps);
  auto right = EvaluatePath(path, 0.4 + eps);
  ASSERT_TRUE(center.has_value() && left.has_value() && right.has_value());

  // The slopes on each side agree.
  const double left_slope = (center->position().x() - left->position().x()) / eps;
  const double right_slope = (right->position().x() - center->position().x()) / eps;
  EXPECT_NEAR(left_slope, right_slope, 1e-3);
}

TEST(CameraPathTest, ShortestYawAndRollCrossWrapWithoutLongSpin) {
  // Turn across the wrap in yaw and roll.
  auto path = MakePath({Key(0, 0, 0, 0, 0, 170, -170), Key(4, 0, 0, 0, 0, -170, 170)});
  auto at1 = EvaluatePath(path, 1);
  ASSERT_TRUE(at1.has_value());
  ExpectClose(at1->angles().yaw(), 175);
  ExpectClose(at1->angles().roll(), -175);

  // Past the seam, the angles keep counting the short way.
  auto at3 = EvaluatePath(path, 3);
  ASSERT_TRUE(at3.has_value());
  ExpectClose(at3->angles().yaw(), 185);
  ExpectClose(at3->angles().roll(), -185);
}

TEST(CameraPathTest, ShortestAnglesStayContinuousAcrossSeamsAndRewinds) {
  // Author views that cross the seam back and forth.
  modlock::CameraPath path;
  *path.add_keys() = Key(0, 0, 0, 0, 0, 170, -170);
  *path.add_keys() = Key(0.7, 0, 0, 0, 0, -175, 175);
  *path.add_keys() = Key(1.8, 0, 0, 0, 0, 175, -175);
  *path.add_keys() = Key(3, 0, 0, 0, 0, -170, 170);

  // Sample densely, tracking the largest step and extremes.
  double max_yaw_delta = 0, max_roll_delta = 0, max_yaw = -1e9, min_roll = 1e9;
  std::vector<modlock::CameraPose> samples;
  for (int i = 0; i <= 3000; ++i) {
    auto evaluated = EvaluatePath(path, i / 1000.0);
    ASSERT_TRUE(evaluated.has_value());
    samples.push_back(*evaluated);
    max_yaw = std::max(max_yaw, evaluated->angles().yaw());
    min_roll = std::min(min_roll, evaluated->angles().roll());
    if (i > 0) {
      max_yaw_delta = std::max(max_yaw_delta,
                               std::abs(evaluated->angles().yaw() - samples[i - 1].angles().yaw()));
      max_roll_delta = std::max(
          max_roll_delta, std::abs(evaluated->angles().roll() - samples[i - 1].angles().roll()));
    }
  }

  // No step jumps, and the angles run past the seam.
  EXPECT_LT(max_yaw_delta, 0.1);
  EXPECT_LT(max_roll_delta, 0.1);
  EXPECT_GT(max_yaw, 180);
  EXPECT_LT(min_roll, -180);

  // Each authored view passes exactly through the evaluated path.
  for (const auto& key : path.keys()) {
    auto evaluated = EvaluatePath(path, key.time());
    ASSERT_TRUE(evaluated.has_value());
    const auto angle_delta = [](double evaluated, double authored) {
      double shifted = std::fmod(evaluated - authored + 180.0, 360.0);
      if (shifted < 0) {
        shifted += 360.0;
      }
      return shifted - 180.0;
    };
    const double yaw_delta = angle_delta(evaluated->angles().yaw(), key.angles().yaw());
    const double roll_delta = angle_delta(evaluated->angles().roll(), key.angles().roll());
    EXPECT_NEAR(yaw_delta, 0, 1e-9);
    EXPECT_NEAR(roll_delta, 0, 1e-9);
  }
}

TEST(CameraPathTest, UnwrappedModePreservesIntentionalMultipleSpins) {
  // Spin twice in yaw and once in roll.
  auto path = MakePath({Key(0), Key(4, 0, 0, 0, 0, 720, -360)}, modlock::INTERPOLATION_SMOOTH,
                       modlock::ROTATION_MODE_UNWRAPPED);
  auto at2 = EvaluatePath(path, 2);
  ASSERT_TRUE(at2.has_value());
  ExpectClose(at2->angles().yaw(), 360);
  ExpectClose(at2->angles().roll(), -180);

  // The end lands on the full spin.
  auto at4 = EvaluatePath(path, 4);
  ASSERT_TRUE(at4.has_value());
  ExpectClose(at4->angles().yaw(), 720);
}

TEST(CameraPathTest, ExactHalfTurnUsesAuthoredDirection) {
  // A positive half turn goes positive.
  auto positive = MakePath({Key(0), Key(2, 0, 0, 0, 0, 180)});
  auto at1 = EvaluatePath(positive, 1);
  ASSERT_TRUE(at1.has_value());
  ExpectClose(at1->angles().yaw(), 90);

  // A negative half turn goes negative.
  auto negative = MakePath({Key(0), Key(2, 0, 0, 0, 0, -180)});
  auto at1n = EvaluatePath(negative, 1);
  ASSERT_TRUE(at1n.has_value());
  ExpectClose(at1n->angles().yaw(), -90);
}

TEST(CameraPathTest, ExtremeFiniteNumbersNeverProduceNonfiniteOutput) {
  auto path = MakePath({Key(0, -1e308, 0, 0, 0, 1e308), Key(1e-310, 1e308), Key(1)});
  for (double time : {0.0, 0.5e-310, 1e-310, 0.5, 1.0}) {
    auto evaluated = EvaluatePath(path, time);
    ASSERT_TRUE(evaluated.has_value());
    EXPECT_TRUE(std::isfinite(evaluated->position().x()));
    EXPECT_TRUE(std::isfinite(evaluated->angles().yaw()));
    EXPECT_TRUE(std::isfinite(evaluated->aspect_ratio()));
  }
}

TEST(CameraPathTest, AspectRatioNeverOvershootsOnUnevenKeys) {
  // Author aspect ratios at uneven times.
  const double times[] = {0, 0.03, 1, 20};
  const double values[] = {1, 1.9, 0.8, 1.5};
  modlock::CameraPath path;
  for (int i = 0; i < 4; ++i) {
    *path.add_keys() = Key(times[i], 0, 0, 0, 0, 0, 0, values[i]);
  }

  // Between each pair, the ratio stays within them.
  for (int i = 0; i < 3; ++i) {
    const double low = std::min(values[i], values[i + 1]);
    const double high = std::max(values[i], values[i + 1]);
    for (int step = 0; step <= 100; ++step) {
      const double t = times[i] + (times[i + 1] - times[i]) * step / 100.0;
      auto evaluated = EvaluatePath(path, t);
      ASSERT_TRUE(evaluated.has_value());
      EXPECT_GE(evaluated->aspect_ratio(), low);
      EXPECT_LE(evaluated->aspect_ratio(), high);
    }
  }
}

TEST(CameraPathTest, StepInterpolationHasExactKeyBoundary) {
  auto path = MakePath({Key(0)}, modlock::INTERPOLATION_STEP);
  auto evaluated = EvaluatePath(path, 1.5);
  ASSERT_TRUE(evaluated.has_value());
  EXPECT_EQ(evaluated->position().x(), 0);
}

TEST(CameraPathTest, ValidationRejectsBadTimesAndAspect) {
  // Duplicate and negative times and an out of range aspect are refused.
  auto duplicate = MakePath({Key(1), Key(1, 5)});
  EXPECT_FALSE(ValidatePath(duplicate).has_value());
  auto negative = MakePath({Key(-1)});
  EXPECT_FALSE(ValidatePath(negative).has_value());
  auto out_of_range = MakePath({Key(0, 0, 0, 0, 0, 0, 0, 9.0)});
  EXPECT_FALSE(ValidatePath(out_of_range).has_value());
}

TEST(IntegrateFlightTest, DiscardsSuspensionBacklog) {
  // A half second step is discarded as backlog.
  modlock::CameraPose pose;
  pose.mutable_position()->set_z(100);
  FlightInput input;
  input.forward = 1;
  IntegrateFlight(pose, input, 0.5, 320, 0.12);
  ExpectClose(pose.position().z(), 100);
}

TEST(IntegrateFlightTest, MovesForwardAlongYawAndClampsPitch) {
  // Fly forward.
  modlock::CameraPose pose;
  FlightInput forward;
  forward.forward = 1;

  // dt 0.1 clamps to 0.05, matching Dolly's integrator.
  IntegrateFlight(pose, forward, 0.1, 320, 0.12);
  ExpectClose(pose.position().x(), 16, 1e-6);
  ExpectClose(pose.position().y(), 0, 1e-6);

  // Look down for a long time; the pitch clamps short of straight down.
  FlightInput look_down;
  look_down.pitch = 1;  // positive pitch looks down
  for (int i = 0; i < 1000; ++i) {
    IntegrateFlight(pose, look_down, 0.05, 320, 0.12);
  }
  ExpectClose(pose.angles().pitch(), 89.9, 1e-6);
}

}  // namespace

}  // namespace modlock::camera
