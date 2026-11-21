#pragma once
// Shared GuideMind types and math. World frame: Y up, metres. Yaw is measured about +Y from +Z, so
// POSITIVE YAW TURNS LEFT (facing +Z, +X is on the left). Pitch is positive looking up.
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <cmath>
#include <cstdint>
#include <string>

#include "capturekit/geometry/geometry.hpp"

namespace capturekit::guide {

using Vec3 = Eigen::Vector3d;
constexpr double kPi = 3.14159265358979323846;
inline double deg2rad(double d) { return d * kPi / 180.0; }
inline double rad2deg(double r) { return r * 180.0 / kPi; }
inline double wrap_pi(double a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}

/// Forward direction (OpenCV +Z of the camera) for yaw/pitch.
inline Vec3 forward(double yaw, double pitch) {
  return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

/// T_world_from_camera (OpenCV axes) looking along yaw/pitch from position p.
inline geometry::PoseWC pose_from_yaw_pitch(const Vec3& p, double yaw, double pitch) {
  Vec3 f = forward(yaw, pitch);
  Vec3 x = f.cross(Vec3::UnitY()).normalized();  // right-handed: right = forward x up (see header note)
  Vec3 y = f.cross(x);
  Eigen::Matrix3d R;
  R.col(0) = x;
  R.col(1) = y;
  R.col(2) = f;
  return geometry::PoseWC(Sophus::SE3d(Eigen::Quaterniond(R), p));
}

inline double yaw_of(const geometry::PoseWC& T) {
  Vec3 f = T.se3().so3() * Vec3::UnitZ();
  return std::atan2(f.x(), f.z());
}
inline double pitch_of(const geometry::PoseWC& T) {
  Vec3 f = T.se3().so3() * Vec3::UnitZ();
  return std::asin(std::clamp(f.y(), -1.0, 1.0));
}

/// Deterministic RNG (SplitMix64): identical streams on every platform, unlike std distributions.
class Rng {
 public:
  explicit Rng(uint64_t seed = 0) : s_(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
  uint64_t next() {
    uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
  double uniform(double a, double b) { return a + (b - a) * uniform(); }
  double normal() {
    double u1 = std::max(uniform(), 1e-12), u2 = uniform();
    return std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2);
  }
  size_t index(size_t n) { return size_t(uniform() * double(n)) % n; }

 private:
  uint64_t s_;
};

}  // namespace capturekit::guide
