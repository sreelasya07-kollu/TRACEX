// Phase 2 geometry tests: projection invariance across conventions, quaternion order,
// image rotation/scale/crop, interpolation. References below use each convention's OWN maths,
// written independently of geometry.cpp (no calls into the adapters for the reference pixel).
#include <gtest/gtest.h>

#include <Eigen/Geometry>
#include <random>

#include "capturekit/geometry/geometry.hpp"

using namespace capturekit::geometry;

namespace {

constexpr double kPxTol = 1e-6;

struct Rig {
  PoseWC T_wc;  // internal pose
  Intrinsics K;
};

Eigen::Quaterniond random_quat(std::mt19937& g) {
  std::normal_distribution<double> n(0, 1);
  Eigen::Quaterniond q(n(g), n(g), n(g), n(g));
  return q.normalized();
}

Rig random_rig(std::mt19937& g, bool distort) {
  std::uniform_real_distribution<double> u(-2, 2);
  Rig r;
  r.T_wc = PoseWC(Sophus::SE3d(random_quat(g), Eigen::Vector3d(u(g), u(g), u(g))));
  r.K.width = 640;
  r.K.height = 480;
  r.K.fx = 500 + 20 * u(g);
  r.K.fy = 505 + 20 * u(g);
  r.K.cx = 319.5 + 3 * u(g);
  r.K.cy = 239.5 + 3 * u(g);
  if (distort) {
    r.K.model = CameraModel::OpenCV;
    r.K.k1 = 0.02 * u(g);
    r.K.k2 = 0.005 * u(g);
    r.K.p1 = 0.001 * u(g);
    r.K.p2 = 0.001 * u(g);
  }
  return r;
}

// Points in front of the camera, expressed in world.
std::vector<Eigen::Vector3d> points_in_view(std::mt19937& g, const PoseWC& T_wc, int n) {
  std::uniform_real_distribution<double> xy(-0.4, 0.4), z(0.5, 6);
  std::vector<Eigen::Vector3d> out;
  for (int i = 0; i < n; ++i) {
    double zz = z(g);
    out.push_back(T_wc * Eigen::Vector3d(xy(g) * zz, xy(g) * zz, zz));
  }
  return out;
}

// Plain OpenCV maths: p_c = R_cw p_w + t_cw; u = fx * x' + cx with Brown-Conrady distortion.
Eigen::Vector2d opencv_project(const Eigen::Matrix3d& R_cw, const Eigen::Vector3d& t_cw,
                               const Intrinsics& K, const Eigen::Vector3d& pw) {
  Eigen::Vector3d pc = R_cw * pw + t_cw;
  double x = pc.x() / pc.z(), y = pc.y() / pc.z();
  if (K.model == CameraModel::OpenCV) {
    double r2 = x * x + y * y, rad = 1 + K.k1 * r2 + K.k2 * r2 * r2;
    double xd = x * rad + 2 * K.p1 * x * y + K.p2 * (r2 + 2 * x * x);
    double yd = y * rad + K.p1 * (r2 + 2 * y * y) + 2 * K.p2 * x * y;
    x = xd;
    y = yd;
  }
  return {K.fx * x + K.cx, K.fy * y + K.cy};
}

// ARKit maths: camera looks down -Z, +Y up. Pixel origin top-left, integer centres (ADR 0003).
Eigen::Vector2d arkit_project(const Eigen::Matrix3d& R_wcam, const Eigen::Vector3d& t_wcam,
                              const Intrinsics& K, const Eigen::Vector3d& pw) {
  Eigen::Vector3d pc = R_wcam.transpose() * (pw - t_wcam);
  return {K.fx * pc.x() / -pc.z() + K.cx, K.fy * -pc.y() / -pc.z() + K.cy};
}

}  // namespace

TEST(GeometryQuat, WxyzRoundTripAndCanonicalSign) {
  Eigen::Quaterniond q(0.5, 0.1, -0.7, 0.3);  // constructor order w,x,y,z
  q.normalize();
  auto a = quat_to_wxyz(q);
  EXPECT_NEAR(a[0], q.w(), 1e-15);
  EXPECT_NEAR(a[1], q.x(), 1e-15);
  EXPECT_NEAR(a[3], q.z(), 1e-15);
  // coeffs() is x,y,z,w: our helper must NOT match it positionally.
  EXPECT_NE(a[0], q.coeffs()[0]);
  auto back = quat_from_wxyz(a);
  EXPECT_TRUE(back.isApprox(q, 1e-15));
  // Negative-w quaternion is canonicalised (same rotation).
  Eigen::Quaterniond neg(-q.w(), -q.x(), -q.y(), -q.z());
  auto c = quat_to_wxyz(neg);
  EXPECT_GE(c[0], 0);
  EXPECT_NEAR(c[2], a[2], 1e-15);
}

TEST(GeometryQuat, PoseWxyzRoundTrip) {
  std::mt19937 g(1);
  for (int i = 0; i < 50; ++i) {
    auto r = random_rig(g, false);
    double t[3], q[4];
    r.T_wc.to_tq(t, q);
    auto back = PoseWC::from_tq(t, q);
    EXPECT_TRUE(back.se3().matrix().isApprox(r.T_wc.se3().matrix(), 1e-12));
  }
}

TEST(GeometryQuat, ArcoreXyzwOrder) {
  // 90 deg about +Y: ARCore delivers (qx,qy,qz,qw) = (0, s, 0, s).
  double s = std::sqrt(0.5);
  ArcorePose a{{1, 2, 3}, {0, s, 0, s}};
  auto T = from_arcore(a);
  // Camera +Z(OpenCV) = -Z(ARCore cam). Rotating -Z by +90 deg about Y gives -X.
  Eigen::Vector3d fwd = T.se3().so3() * Eigen::Vector3d::UnitZ();
  EXPECT_TRUE(fwd.isApprox(Eigen::Vector3d(-1, 0, 0), 1e-12));
  EXPECT_TRUE(T.translation().isApprox(Eigen::Vector3d(1, 2, 3)));
}

TEST(GeometryInvariance, ArkitToInternal) {
  std::mt19937 g(2);
  for (int trial = 0; trial < 30; ++trial) {
    auto r = random_rig(g, false);
    // Build the ARKit pose with raw maths (independent of axis_flip).
    Eigen::Matrix3d F = Eigen::Vector3d(1, -1, -1).asDiagonal();
    Eigen::Matrix3d R_arkit = r.T_wc.se3().rotationMatrix() * F;
    Eigen::Vector3d t = r.T_wc.translation();
    PoseWorldFromGlCamera arkit(Sophus::SE3d(Eigen::Quaterniond(R_arkit), t));
    auto T = from_arkit(arkit);
    for (auto& pw : points_in_view(g, r.T_wc, 20)) {
      Eigen::Vector2d ref = arkit_project(R_arkit, t, r.K, pw);
      EXPECT_LT((project(r.K, T, pw).value() - ref).norm(), kPxTol);
    }
  }
}

TEST(GeometryInvariance, OpenCvExtrinsics) {
  std::mt19937 g(3);
  for (int trial = 0; trial < 30; ++trial) {
    auto r = random_rig(g, trial % 2 == 1);
    auto T_cw = r.T_wc.inverse();
    for (auto& pw : points_in_view(g, r.T_wc, 20)) {
      Eigen::Vector2d ref =
          opencv_project(T_cw.se3().rotationMatrix(), T_cw.translation(), r.K, pw);
      EXPECT_LT((project(r.K, r.T_wc, pw).value() - ref).norm(), kPxTol);
    }
  }
}

TEST(GeometryInvariance, Colmap) {
  std::mt19937 g(4);
  for (int trial = 0; trial < 30; ++trial) {
    auto r = random_rig(g, trial % 2 == 1);
    auto c = to_colmap(r.T_wc, r.K);
    // COLMAP maths: R from qvec(w,x,y,z), pixel centre of top-left = (0.5,0.5).
    Eigen::Quaterniond q(c.qvec_wxyz[0], c.qvec_wxyz[1], c.qvec_wxyz[2], c.qvec_wxyz[3]);
    Eigen::Vector3d t(c.tvec[0], c.tvec[1], c.tvec[2]);
    Intrinsics Kc = r.K;
    Kc.fx = c.params[0];
    Kc.fy = c.params[1];
    Kc.cx = c.params[2];
    Kc.cy = c.params[3];
    if (r.K.model == CameraModel::OpenCV) {
      EXPECT_EQ(c.model, "OPENCV");
      Kc.k1 = c.params[4];
      Kc.k2 = c.params[5];
      Kc.p1 = c.params[6];
      Kc.p2 = c.params[7];
    } else {
      EXPECT_EQ(c.model, "PINHOLE");
    }
    for (auto& pw : points_in_view(g, r.T_wc, 20)) {
      Eigen::Vector2d colmap_px = opencv_project(q.toRotationMatrix(), t, Kc, pw);
      Eigen::Vector2d ours = project(r.K, r.T_wc, pw).value();
      EXPECT_LT((colmap_px - (ours + Eigen::Vector2d(0.5, 0.5))).norm(), kPxTol);
    }
  }
}

TEST(GeometryInvariance, OpenGl) {
  std::mt19937 g(5);
  for (int trial = 0; trial < 30; ++trial) {
    auto r = random_rig(g, false);
    auto gl = to_opengl(r.T_wc, r.K, 0.05, 100.0);
    Eigen::Matrix4d P = Eigen::Map<const Eigen::Matrix4d>(gl.projection_col_major.data());
    Eigen::Matrix4d V = gl.view.se3().matrix();
    for (auto& pw : points_in_view(g, r.T_wc, 20)) {
      Eigen::Vector4d clip = P * V * pw.homogeneous();
      Eigen::Vector3d ndc = clip.head<3>() / clip.w();
      EXPECT_GT(ndc.z(), -1);
      EXPECT_LT(ndc.z(), 1);
      // Viewport: window x = (ndc.x+1)/2*W (pixel centres at +0.5), window y is bottom-up.
      double u = (ndc.x() + 1) / 2 * r.K.width - 0.5;
      double v = r.K.height - (ndc.y() + 1) / 2 * r.K.height - 0.5;
      Eigen::Vector2d ours = project(r.K, r.T_wc, pw).value();
      EXPECT_LT((Eigen::Vector2d(u, v) - ours).norm(), kPxTol);
    }
  }
}

TEST(GeometryInvariance, AxisFlipIsInvolution) {
  auto F = axis_flip();
  auto FF = F.se3() * F.se3();
  EXPECT_TRUE(FF.matrix().isApprox(Eigen::Matrix4d::Identity(), 1e-15));
}

class RotateImage : public ::testing::TestWithParam<int> {};

TEST_P(RotateImage, PixelsFollowTheImageRotation) {
  int deg = GetParam();
  std::mt19937 g(6 + deg);
  for (int trial = 0; trial < 20; ++trial) {
    auto r = random_rig(g, trial % 2 == 1);
    auto [T2, K2] = rotate_image(r.T_wc, r.K, deg);
    const double W = r.K.width, H = r.K.height;
    for (auto& pw : points_in_view(g, r.T_wc, 20)) {
      Eigen::Vector2d p = project(r.K, r.T_wc, pw).value();
      // Rotating the pixel grid clockwise by deg (integer-centre convention).
      Eigen::Vector2d e;
      switch (deg) {
        case 0: e = p; break;
        case 90: e = {H - 1 - p.y(), p.x()}; break;
        case 180: e = {W - 1 - p.x(), H - 1 - p.y()}; break;
        default: e = {p.y(), W - 1 - p.x()}; break;
      }
      EXPECT_LT((project(K2, T2, pw).value() - e).norm(), kPxTol) << deg;
    }
    EXPECT_EQ(K2.width, (deg % 180 == 0) ? r.K.width : r.K.height);
    // The optical axis is unchanged.
    Eigen::Vector3d z1 = r.T_wc.se3().so3() * Eigen::Vector3d::UnitZ();
    Eigen::Vector3d z2 = T2.se3().so3() * Eigen::Vector3d::UnitZ();
    EXPECT_TRUE(z1.isApprox(z2, 1e-12));
  }
}
INSTANTIATE_TEST_SUITE_P(Geometry, RotateImage, ::testing::Values(0, 90, 180, 270));

TEST(GeometryRotate, RejectsNonQuarterTurns) {
  Intrinsics K{640, 480, 500, 500, 319.5, 239.5};
  EXPECT_THROW(rotate_image(PoseWC(), K, 45), std::invalid_argument);
  auto [T, K4] = rotate_image(PoseWC(), K, -90);  // -90 == 270
  auto [T3, K3] = rotate_image(PoseWC(), K, 270);
  EXPECT_DOUBLE_EQ(K4.cx, K3.cx);
}

TEST(GeometryScale, ScaleAndCropFollowPixelGrid) {
  std::mt19937 g(7);
  for (double s : {0.5, 0.25, 2.0, 3.0}) {
    auto r = random_rig(g, false);
    auto K2 = scale_intrinsics(r.K, uint32_t(r.K.width * s), uint32_t(r.K.height * s));
    for (auto& pw : points_in_view(g, r.T_wc, 10)) {
      Eigen::Vector2d p = project(r.K, r.T_wc, pw).value();
      Eigen::Vector2d e = s * (p.array() + 0.5).matrix() - Eigen::Vector2d(0.5, 0.5);
      EXPECT_LT((project(K2, r.T_wc, pw).value() - e).norm(), kPxTol) << s;
    }
  }
  auto r = random_rig(g, false);
  auto Kc = crop_intrinsics(r.K, 40, 30, 400, 300);
  EXPECT_EQ(Kc.width, 400u);
  for (auto& pw : points_in_view(g, r.T_wc, 10)) {
    Eigen::Vector2d d = project(r.K, r.T_wc, pw).value() - project(Kc, r.T_wc, pw).value();
    EXPECT_LT((d - Eigen::Vector2d(40, 30)).norm(), kPxTol);
  }
}

TEST(GeometryProject, BehindCameraIsEmpty) {
  Intrinsics K{640, 480, 500, 500, 319.5, 239.5};
  EXPECT_FALSE(project(K, PoseWC(), Eigen::Vector3d(0, 0, -1)).has_value());
}

// ---- Interpolation ----

TEST(GeometryInterp, EndpointsMidpointAndRefusal) {
  PoseTrack track;
  Eigen::Quaterniond qa = Eigen::Quaterniond::Identity();
  Eigen::Quaterniond qb(Eigen::AngleAxisd(0.6, Eigen::Vector3d::UnitY()));
  track.push(1'000'000'000, PoseWC(Sophus::SE3d(qa, Eigen::Vector3d(0, 0, 0))));
  track.push(1'100'000'000, PoseWC(Sophus::SE3d(qb, Eigen::Vector3d(1, 0, 0))));
  track.push(1'500'000'000, PoseWC(Sophus::SE3d(qb, Eigen::Vector3d(2, 0, 0))));

  const int64_t gap = 150'000'000;
  auto a = track.interpolate_at(1'000'000'000, gap);
  ASSERT_TRUE(a);
  EXPECT_TRUE(a->translation().isZero(1e-15));
  auto b = track.interpolate_at(1'100'000'000, gap);
  ASSERT_TRUE(b);
  EXPECT_TRUE(b->translation().isApprox(Eigen::Vector3d(1, 0, 0)));
  auto m = track.interpolate_at(1'050'000'000, gap);
  ASSERT_TRUE(m);
  EXPECT_TRUE(m->translation().isApprox(Eigen::Vector3d(0.5, 0, 0), 1e-12));
  EXPECT_NEAR(Eigen::AngleAxisd(m->se3().unit_quaternion()).angle(), 0.3, 1e-12);
  // Refuse: gap 400 ms > 150 ms; before first; after last.
  EXPECT_FALSE(track.interpolate_at(1'300'000'000, gap));
  EXPECT_FALSE(track.interpolate_at(999'999'999, gap));
  EXPECT_FALSE(track.interpolate_at(1'500'000'001, gap));
  EXPECT_TRUE(track.interpolate_at(1'300'000'000, 500'000'000));
}

TEST(GeometryInterp, NearestWithTolerance) {
  PoseTrack track;
  for (int64_t i = 0; i < 10; ++i) track.push(i * 33'000'000, PoseWC());
  EXPECT_EQ(track.nearest(65'000'000, 5'000'000), std::optional<size_t>(2));
  EXPECT_EQ(track.nearest(50'000'000, 5'000'000), std::nullopt);
  EXPECT_EQ(track.nearest(-1, 5'000'000), std::optional<size_t>(0));
  EXPECT_EQ(track.nearest(400'000'000, 5'000'000), std::nullopt);
  EXPECT_EQ(PoseTrack().nearest(0, 1), std::nullopt);
}

TEST(GeometryInterp, RejectsNonMonotonicPush) {
  PoseTrack track;
  track.push(10, PoseWC());
  EXPECT_THROW(track.push(10, PoseWC()), std::invalid_argument);
}

TEST(GeometryInterp, ShortestArcAcrossSignFlip) {
  Eigen::Quaterniond qa(Eigen::AngleAxisd(0.1, Eigen::Vector3d::UnitZ()));
  Eigen::Quaterniond qb(Eigen::AngleAxisd(0.3, Eigen::Vector3d::UnitZ()));
  qb.coeffs() *= -1;  // same rotation, opposite hemisphere
  auto m = interpolate(PoseWC(Sophus::SE3d(qa, Eigen::Vector3d::Zero())),
                       PoseWC(Sophus::SE3d(qb, Eigen::Vector3d::Zero())), 0.5);
  EXPECT_NEAR(Eigen::AngleAxisd(m.se3().unit_quaternion()).angle(), 0.2, 1e-12);
}
