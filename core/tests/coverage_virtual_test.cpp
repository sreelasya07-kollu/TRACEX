// Virtual coverage grids (Phase 9 step 7) follow their surface: extent changes keep the cells, an orientation or
// plane-offset change beyond tolerance rebuilds the grid on the new axes (cells of the old plane are dropped).
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "capturekit/guide/coverage.hpp"

using namespace capturekit;
using namespace capturekit::guide;

namespace {

geometry::Intrinsics test_K() {
  geometry::Intrinsics K;
  K.width = 640, K.height = 480, K.fx = K.fy = 500, K.cx = 319.5, K.cy = 239.5;
  return K;
}

Obs obs_looking(const Vec3& from, const Vec3& at, double dyaw = 0, double dpitch = 0) {
  Obs o;
  const double yaw = std::atan2(at.x() - from.x(), at.z() - from.z()) + dyaw;
  const double hd = std::hypot(at.x() - from.x(), at.z() - from.z());
  const double pitch = std::atan2(at.y() - from.y(), hd) + dpitch;
  o.pose = pose_from_yaw_pitch(from, yaw, pitch);
  o.K = test_K(), o.t_ns = 1000000000, o.sharp_norm = 1.0, o.keyframe = true;
  return o;
}

// Wall on x = 0 facing +x: u = +Z (4 m), v = +Y (2.5 m), centre at mid height.
VirtualSurface wall(double eu = 4.0, double ev = 2.5) {
  VirtualSurface s;
  s.id = "v:wall";
  s.c = {0.0, 1.25, 0.0}, s.n = Vec3::UnitX(), s.u = Vec3::UnitZ(), s.v = Vec3::UnitY();
  s.eu = eu, s.ev = ev;
  return s;
}

VirtualSurface turned_about_y(VirtualSurface s, double deg) {
  const Eigen::AngleAxisd R(deg * M_PI / 180.0, Vec3::UnitY());
  s.n = R * s.n, s.u = R * s.u, s.v = R * s.v;
  return s;
}

void look_at_wall(Coverage& c, const Vec3& wall_pt, const Vec3& from) {
  for (double dy : {-0.25, 0.0, 0.25})
    for (double dp : {-0.18, 0.18}) c.observe(obs_looking(from, wall_pt, dy, dp), nullptr, nullptr);
}

double seen(const Coverage& c) { return c.stats("v:wall").seen_fraction(); }

}  // namespace

TEST(CoverageVirtual, ExtentChangeKeepsSeenCells) {
  GuideParams gp;
  Coverage c(gp);
  c.sync_virtual({wall()});
  look_at_wall(c, {0, 1.25, 0}, {5.5, 1.25, 0});
  const double before = seen(c);
  ASSERT_GT(before, 0.5);
  const size_t valid_before = c.stats("v:wall").valid;
  c.sync_virtual({wall(4.4, 2.5)});  // wall refit slightly longer: same axes
  EXPECT_GT(c.stats("v:wall").valid, valid_before);
  EXPECT_GT(c.stats("v:wall").seen, 0u);  // old cells kept
  c.sync_virtual({turned_about_y(wall(), 0.5)});  // below tolerance: jitter must not erase evidence
  EXPECT_GT(c.stats("v:wall").seen, 0u);
}

TEST(CoverageVirtual, OrientationChangeRebuildsOnNewAxes) {
  GuideParams gp;
  Coverage c(gp);
  c.sync_virtual({wall()});
  look_at_wall(c, {0, 1.25, 0}, {5.5, 1.25, 0});
  ASSERT_GT(seen(c), 0.5);

  const VirtualSurface t = turned_about_y(wall(), 12.0);
  c.sync_virtual({t});
  EXPECT_GT(c.stats("v:wall").valid, 100u);  // refit on the new rectangle
  EXPECT_EQ(c.stats("v:wall").seen, 0u);     // cells of the old plane dropped

  // Cells now lie on the turned plane: looking at it along its normal sees most of it.
  const Vec3 from = t.c + t.n * 5.5;
  look_at_wall(c, t.c, from);
  EXPECT_GT(seen(c), 0.5);
}

TEST(CoverageVirtual, PlaneOffsetChangeRebuilds) {
  GuideParams gp;
  Coverage c(gp);
  c.sync_virtual({wall()});
  look_at_wall(c, {0, 1.25, 0}, {5.5, 1.25, 0});
  ASSERT_GT(c.stats("v:wall").seen, 0u);
  VirtualSurface moved = wall();
  moved.c.x() += 0.5;  // wall refit half a metre further into the room
  c.sync_virtual({moved});
  EXPECT_EQ(c.stats("v:wall").seen, 0u);
  EXPECT_GT(c.stats("v:wall").valid, 100u);
}

TEST(CoverageVirtual, SmallTiltRebuildsOnlyWhenExtentCornersDrift) {
  GuideParams gp;  // cell 0.10 m: rebuild when a corner is > 5 cm off the old plane
  {
    Coverage c(gp);
    c.sync_virtual({wall()});  // 4 m long: half-length 2 m
    look_at_wall(c, {0, 1.25, 0}, {5.5, 1.25, 0});
    ASSERT_GT(c.stats("v:wall").seen, 0u);
    c.sync_virtual({turned_about_y(wall(), 1.0)});  // 1 deg: corners 3.5 cm off -> keep
    EXPECT_GT(c.stats("v:wall").seen, 0u);
    c.sync_virtual({turned_about_y(wall(), 1.9)});  // under the 2 deg rule, corners 6.6 cm off -> rebuild
    EXPECT_EQ(c.stats("v:wall").seen, 0u);
  }
  {
    Coverage c(gp);
    c.sync_virtual({wall(8.0, 2.5)});  // long wall: the same 1 deg tilt drifts 7 cm at the ends
    look_at_wall(c, {0, 1.25, 0}, {5.5, 1.25, 0});
    ASSERT_GT(c.stats("v:wall").seen, 0u);
    c.sync_virtual({turned_about_y(wall(8.0, 2.5), 1.0)});
    EXPECT_EQ(c.stats("v:wall").seen, 0u);
  }
  {
    Coverage c(gp);  // a wall that keeps its plane but grows keeps its evidence
    c.sync_virtual({wall()});
    look_at_wall(c, {0, 1.25, 0}, {5.5, 1.25, 0});
    ASSERT_GT(c.stats("v:wall").seen, 0u);
    VirtualSurface g = wall(6.0, 2.5);
    g.c.z() += 0.5;  // longer and shifted in-plane
    c.sync_virtual({g});
    EXPECT_GT(c.stats("v:wall").seen, 0u);
  }
}
