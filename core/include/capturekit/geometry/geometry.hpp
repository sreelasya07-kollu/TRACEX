#pragma once
// Geometry core (Phase 2). Conventions: docs/program-spec/conventions.md.
// Every pose is a frame-named Pose<To, From> = T_to_from (maps points in From to To).
// Small named steps (axis_flip, inverse, rotate_image, scale/crop_intrinsics) are composed once
// per adapter (from_arkit, from_arcore, to_colmap, to_opengl).
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <array>
#include <cstdint>
#include <optional>
#include <sophus/se3.hpp>
#include <string>
#include <utility>
#include <vector>

namespace capturekit::geometry {

/// World = Y-up gravity-aligned session frame. Camera = OpenCV (X right, Y down, Z forward).
/// GlCamera = ARKit/ARCore/OpenGL camera axes (X right, Y up, looks down -Z).
enum class Frame { World, Camera, GlCamera };

template <Frame To, Frame From>
class Pose {
 public:
  Pose() = default;
  explicit Pose(const Sophus::SE3d& T) : T_(T) {}

  const Sophus::SE3d& se3() const { return T_; }
  Eigen::Vector3d translation() const { return T_.translation(); }
  Eigen::Quaterniond rotation() const { return T_.unit_quaternion(); }
  Pose<From, To> inverse() const { return Pose<From, To>(T_.inverse()); }

  template <Frame Mid>
  friend Pose<To, Mid> operator*(const Pose& a, const Pose<From, Mid>& b) {
    return Pose<To, Mid>(a.T_ * b.se3());
  }
  Eigen::Vector3d operator*(const Eigen::Vector3d& p_from) const { return T_ * p_from; }

  /// On-disk form: translation [x,y,z] + quaternion [w,x,y,z] with w >= 0.
  void to_tq(double t[3], double q_wxyz[4]) const;
  static Pose from_tq(const double t[3], const double q_wxyz[4]);

 private:
  Sophus::SE3d T_;
};

using PoseWC = Pose<Frame::World, Frame::Camera>;             // T_world_from_camera (internal)
using PoseCW = Pose<Frame::Camera, Frame::World>;             // cam_from_world (OpenCV/COLMAP)
using PoseWorldFromGlCamera = Pose<Frame::World, Frame::GlCamera>;  // ARKit/ARCore pose
using PoseGlCameraFromWorld = Pose<Frame::GlCamera, Frame::World>;  // OpenGL view
using PoseGlFromCamera = Pose<Frame::GlCamera, Frame::Camera>;

// ---- Quaternion order guards (Eigen coeffs() is x,y,z,w; never serialise it) ----
std::array<double, 4> quat_to_wxyz(const Eigen::Quaterniond& q);  // normalised, w >= 0
Eigen::Quaterniond quat_from_wxyz(const double q_wxyz[4]);
inline Eigen::Quaterniond quat_from_wxyz(const std::array<double, 4>& q) {
  return quat_from_wxyz(q.data());
}

// ---- Intrinsics ----
enum class CameraModel { Pinhole, OpenCV };

/// Pixels of the stored image; OpenCV pixel convention (top-left pixel centre = (0,0)).
struct Intrinsics {
  uint32_t width = 0, height = 0;
  double fx = 0, fy = 0, cx = 0, cy = 0;
  CameraModel model = CameraModel::Pinhole;
  double k1 = 0, k2 = 0, p1 = 0, p2 = 0;  // OpenCV model only
};

/// Projects a world point; nullopt if it is not in front of the camera (z <= 1e-9).
std::optional<Eigen::Vector2d> project(const Intrinsics& K, const PoseWC& T_world_from_camera,
                                       const Eigen::Vector3d& p_world);

// ---- Steps ----
/// diag(1,-1,-1): T_glcamera_from_camera (its own inverse).
PoseGlFromCamera axis_flip();
/// Rotate the stored image clockwise by deg in {0, ±90, 180, 270}: updates pose and intrinsics.
std::pair<PoseWC, Intrinsics> rotate_image(const PoseWC& T_world_from_camera, const Intrinsics& K,
                                           int deg_cw);
/// Resample to new resolution (pixel-centre aware): cx' = s(cx + 0.5) - 0.5.
Intrinsics scale_intrinsics(const Intrinsics& K, uint32_t new_width, uint32_t new_height);
Intrinsics crop_intrinsics(const Intrinsics& K, uint32_t x0, uint32_t y0, uint32_t width,
                           uint32_t height);

// ---- Adapters (compose the steps once) ----
/// ARKit ARCamera.transform (world_from_camera, GL camera axes) -> internal.
PoseWC from_arkit(const PoseWorldFromGlCamera& T_world_from_arkit_camera);

/// ARCore Camera.getPose() (sensor-oriented, NOT display-oriented). Quaternion order x,y,z,w.
struct ArcorePose {
  double t[3];
  double q_xyzw[4];
};
PoseWC from_arcore(const ArcorePose& pose);

struct ColmapView {
  std::string model;              // "PINHOLE" or "OPENCV"
  uint32_t width = 0, height = 0;
  std::vector<double> params;     // fx fy cx cy [k1 k2 p1 p2], cx/cy in COLMAP pixel convention
  std::array<double, 4> qvec_wxyz{};  // cam_from_world rotation, images.txt order
  std::array<double, 3> tvec{};
  PoseCW cam_from_world;
};
/// Inverts the pose and shifts cx, cy by +0.5 (COLMAP pixel centre, ADR 0003).
ColmapView to_colmap(const PoseWC& T_world_from_camera, const Intrinsics& K);

struct GlView {
  PoseGlCameraFromWorld view;
  /// clip_from_glcamera, column-major (glUniformMatrix4fv order), viewport = full stored image.
  std::array<double, 16> projection_col_major{};
};
GlView to_opengl(const PoseWC& T_world_from_camera, const Intrinsics& K, double z_near,
                 double z_far);

// ---- Time alignment ----
/// SLERP rotation + linear translation, alpha in [0,1]. Shortest arc.
PoseWC interpolate(const PoseWC& a, const PoseWC& b, double alpha);

/// Poses sorted by strictly increasing t_ns.
class PoseTrack {
 public:
  void push(int64_t t_ns, const PoseWC& T);  // throws std::invalid_argument if not increasing
  size_t size() const { return t_.size(); }
  int64_t time(size_t i) const { return t_[i]; }
  const PoseWC& pose(size_t i) const { return p_[i]; }

  /// Index of the sample closest to t_ns, if within tolerance_ns.
  std::optional<size_t> nearest(int64_t t_ns, int64_t tolerance_ns) const;
  /// Interpolated pose at t_ns. Refuses to extrapolate and refuses brackets wider than max_gap_ns.
  std::optional<PoseWC> interpolate_at(int64_t t_ns, int64_t max_gap_ns) const;

 private:
  std::vector<int64_t> t_;
  std::vector<PoseWC> p_;
};

// ---- Template definitions ----
template <Frame To, Frame From>
void Pose<To, From>::to_tq(double t[3], double q_wxyz[4]) const {
  auto tr = T_.translation();
  for (int i = 0; i < 3; ++i) t[i] = tr[i];
  auto q = quat_to_wxyz(T_.unit_quaternion());
  for (int i = 0; i < 4; ++i) q_wxyz[i] = q[size_t(i)];
}

template <Frame To, Frame From>
Pose<To, From> Pose<To, From>::from_tq(const double t[3], const double q_wxyz[4]) {
  return Pose(Sophus::SE3d(quat_from_wxyz(q_wxyz), Eigen::Vector3d(t[0], t[1], t[2])));
}

}  // namespace capturekit::geometry
