#include "capturekit/geometry/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "capturekit/version.hpp"

namespace capturekit::geometry {

std::array<double, 4> quat_to_wxyz(const Eigen::Quaterniond& q_in) {
  Eigen::Quaterniond q = q_in.normalized();
  double s = q.w() < 0 ? -1.0 : 1.0;
  return {s * q.w(), s * q.x(), s * q.y(), s * q.z()};
}

Eigen::Quaterniond quat_from_wxyz(const double q[4]) {
  return Eigen::Quaterniond(q[0], q[1], q[2], q[3]).normalized();
}

std::optional<Eigen::Vector2d> project(const Intrinsics& K, const PoseWC& T_wc,
                                       const Eigen::Vector3d& p_world) {
  Eigen::Vector3d pc = T_wc.se3().inverse() * p_world;
  if (pc.z() <= 1e-9) return std::nullopt;
  double x = pc.x() / pc.z(), y = pc.y() / pc.z();
  if (K.model == CameraModel::OpenCV) {
    double r2 = x * x + y * y, rad = 1 + K.k1 * r2 + K.k2 * r2 * r2;
    double xd = x * rad + 2 * K.p1 * x * y + K.p2 * (r2 + 2 * x * x);
    double yd = y * rad + K.p1 * (r2 + 2 * y * y) + 2 * K.p2 * x * y;
    x = xd;
    y = yd;
  }
  return Eigen::Vector2d(K.fx * x + K.cx, K.fy * y + K.cy);
}

PoseGlFromCamera axis_flip() {
  // diag(1,-1,-1) = 180 deg about X: quaternion (w,x,y,z) = (0,1,0,0).
  return PoseGlFromCamera(Sophus::SE3d(Eigen::Quaterniond(0, 1, 0, 0), Eigen::Vector3d::Zero()));
}

namespace {

// One clockwise quarter turn of the stored image (conventions.md "Images").
std::pair<PoseWC, Intrinsics> rotate_cw90(const PoseWC& T, const Intrinsics& K) {
  Intrinsics R = K;
  R.width = K.height;
  R.height = K.width;
  R.fx = K.fy;
  R.fy = K.fx;
  R.cx = double(K.height) - 1.0 - K.cy;
  R.cy = K.cx;
  R.p1 = K.p2;  // tangential terms follow the axis permutation x' = -y, y' = x
  R.p2 = -K.p1;
  // R_camera_from_camera' = Rz(-90 deg).
  Sophus::SE3d T_c_c2(Eigen::Quaterniond(Eigen::AngleAxisd(-M_PI / 2, Eigen::Vector3d::UnitZ())),
                      Eigen::Vector3d::Zero());
  return {PoseWC(T.se3() * T_c_c2), R};
}

}  // namespace

std::pair<PoseWC, Intrinsics> rotate_image(const PoseWC& T, const Intrinsics& K, int deg_cw) {
  if (deg_cw % 90 != 0) throw std::invalid_argument("rotate_image: deg must be a multiple of 90");
  int turns = ((deg_cw / 90) % 4 + 4) % 4;
  std::pair<PoseWC, Intrinsics> out{T, K};
  for (int i = 0; i < turns; ++i) out = rotate_cw90(out.first, out.second);
  return out;
}

Intrinsics scale_intrinsics(const Intrinsics& K, uint32_t new_width, uint32_t new_height) {
  if (K.width == 0 || K.height == 0) throw std::invalid_argument("scale_intrinsics: empty size");
  double sx = double(new_width) / K.width, sy = double(new_height) / K.height;
  Intrinsics R = K;
  R.width = new_width;
  R.height = new_height;
  R.fx = K.fx * sx;
  R.fy = K.fy * sy;
  R.cx = sx * (K.cx + 0.5) - 0.5;
  R.cy = sy * (K.cy + 0.5) - 0.5;
  return R;
}

Intrinsics crop_intrinsics(const Intrinsics& K, uint32_t x0, uint32_t y0, uint32_t width,
                           uint32_t height) {
  if (x0 + width > K.width || y0 + height > K.height)
    throw std::invalid_argument("crop_intrinsics: crop exceeds image");
  Intrinsics R = K;
  R.width = width;
  R.height = height;
  R.cx = K.cx - x0;
  R.cy = K.cy - y0;
  return R;
}

PoseWC from_arkit(const PoseWorldFromGlCamera& T_w_gl) { return T_w_gl * axis_flip(); }

PoseWC from_arcore(const ArcorePose& a) {
  Eigen::Quaterniond q(a.q_xyzw[3], a.q_xyzw[0], a.q_xyzw[1], a.q_xyzw[2]);  // (w,x,y,z) ctor
  return from_arkit(PoseWorldFromGlCamera(
      Sophus::SE3d(q.normalized(), Eigen::Vector3d(a.t[0], a.t[1], a.t[2]))));
}

ColmapView to_colmap(const PoseWC& T_wc, const Intrinsics& K) {
  ColmapView v;
  v.cam_from_world = T_wc.inverse();
  v.qvec_wxyz = quat_to_wxyz(v.cam_from_world.rotation());
  auto t = v.cam_from_world.translation();
  v.tvec = {t.x(), t.y(), t.z()};
  v.width = K.width;
  v.height = K.height;
  v.params = {K.fx, K.fy, K.cx + 0.5, K.cy + 0.5};
  if (K.model == CameraModel::OpenCV) {
    v.model = "OPENCV";
    v.params.insert(v.params.end(), {K.k1, K.k2, K.p1, K.p2});
  } else {
    v.model = "PINHOLE";
  }
  return v;
}

GlView to_opengl(const PoseWC& T_wc, const Intrinsics& K, double n, double f) {
  if (K.model != CameraModel::Pinhole) throw std::invalid_argument("to_opengl: pinhole only");
  GlView g;
  g.view = axis_flip() * T_wc.inverse();
  const double W = K.width, H = K.height;
  Eigen::Matrix4d P = Eigen::Matrix4d::Zero();
  P(0, 0) = 2 * K.fx / W;
  P(0, 2) = 1 - 2 * (K.cx + 0.5) / W;
  P(1, 1) = 2 * K.fy / H;
  P(1, 2) = 2 * (K.cy + 0.5) / H - 1;
  P(2, 2) = -(f + n) / (f - n);
  P(2, 3) = -2 * f * n / (f - n);
  P(3, 2) = -1;
  Eigen::Map<Eigen::Matrix4d>(g.projection_col_major.data()) = P;  // Eigen default: col-major
  return g;
}

PoseWC interpolate(const PoseWC& a, const PoseWC& b, double alpha) {
  Eigen::Quaterniond q = a.rotation().slerp(alpha, b.rotation());  // Eigen slerp: shortest arc
  Eigen::Vector3d t = (1 - alpha) * a.translation() + alpha * b.translation();
  return PoseWC(Sophus::SE3d(q.normalized(), t));
}

void PoseTrack::push(int64_t t_ns, const PoseWC& T) {
  if (!t_.empty() && t_ns <= t_.back())
    throw std::invalid_argument("PoseTrack::push: t_ns must be strictly increasing");
  t_.push_back(t_ns);
  p_.push_back(T);
}

std::optional<size_t> PoseTrack::nearest(int64_t t_ns, int64_t tol) const {
  if (t_.empty()) return std::nullopt;
  auto it = std::lower_bound(t_.begin(), t_.end(), t_ns);
  size_t best = 0;
  int64_t best_d = INT64_MAX;
  for (auto j : {it - t_.begin() - 1, it - t_.begin()}) {
    if (j < 0 || size_t(j) >= t_.size()) continue;
    int64_t d = t_[size_t(j)] > t_ns ? t_[size_t(j)] - t_ns : t_ns - t_[size_t(j)];
    if (d < best_d) best_d = d, best = size_t(j);
  }
  if (best_d > tol) return std::nullopt;
  return best;
}

std::optional<PoseWC> PoseTrack::interpolate_at(int64_t t_ns, int64_t max_gap_ns) const {
  if (t_.empty() || t_ns < t_.front() || t_ns > t_.back()) return std::nullopt;
  auto it = std::lower_bound(t_.begin(), t_.end(), t_ns);
  size_t i = size_t(it - t_.begin());
  if (t_[i] == t_ns) return p_[i];
  int64_t ta = t_[i - 1], tb = t_[i];
  if (tb - ta > max_gap_ns) return std::nullopt;
  return interpolate(p_[i - 1], p_[i], double(t_ns - ta) / double(tb - ta));
}

}  // namespace capturekit::geometry

namespace capturekit {
const char* core_version() noexcept { return "0.0.1"; }
}  // namespace capturekit
