// Android brief B1: OverlayModel snapshot + "overlay" event. Golden JSON comes from a fixed-seed sim run.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <fstream>
#include <set>
#include <sstream>

#include "capturekit/guide/engine.hpp"
#include "capturekit/guide/overlay.hpp"
#include "capturekit/guide/sim.hpp"

using namespace capturekit;
using namespace capturekit::guide;
namespace fs = std::filesystem;

namespace {
size_t words(const std::string& s) {
  std::istringstream is(s);
  size_t n = 0;
  for (std::string w; is >> w;) ++n;
  return n;
}

bundle::PoseSample pose_at(int64_t t, double x, double z, double yaw, int state = 1) {
  auto T = pose_from_yaw_pitch({x, 1.4, z}, yaw, 0);
  bundle::PoseSample p;
  p.t_ns = t;
  for (int i = 0; i < 3; ++i) p.t_world_from_camera[i] = T.translation()[i];
  auto q = T.se3().unit_quaternion();
  p.q_world_from_camera_wxyz[0] = q.w(), p.q_world_from_camera_wxyz[1] = q.x();
  p.q_world_from_camera_wxyz[2] = q.y(), p.q_world_from_camera_wxyz[3] = q.z();
  p.tracking_state = static_cast<bundle::TrackingState>(state);
  return p;
}

bundle::PlaneRecord wall(const std::string& id, Vec3 c, Vec3 n, Vec3 u, double eu, double ev) {
  bundle::PlaneRecord p;
  p.id = id, p.kind = "wall";
  for (int i = 0; i < 3; ++i) p.center[i] = c[i], p.normal[i] = n[i], p.axis_u[i] = u[i];
  p.extent_u = eu, p.extent_v = ev;
  return p;
}

std::string golden_run() {
  sim::Config c;
  c.room = "box";
  c.policy = "planner";
  c.seed = 1;
  c.max_s = 60;
  c.overlay_every_s = 5;
  return sim::simulate(c);
}

// The overlay_samples array of a sim report, one JSON object per line.
std::string samples_of(const std::string& report) {
  auto a = report.find("\"overlay_samples\":[");
  auto b = report.find("],\"", a);
  std::string arr = report.substr(a + 19, b - a - 19), out;
  for (size_t i = 0; i < arr.size(); ++i) {
    out += arr[i];
    if (arr[i] == '}' && i + 1 < arr.size() && arr[i + 1] == ',') out += '\n', ++i;
  }
  return out + "\n";
}
}  // namespace

TEST(Overlay, CatalogTextsAreShortSpeakStyle) {
  auto ids = why_ids();
  ASSERT_GE(ids.size(), 10u);
  for (auto& id : ids) {
    std::string t = why_line(id);
    EXPECT_FALSE(t.empty()) << id;
    EXPECT_LE(words(t), 8u) << id << ": " << t;
  }
  EXPECT_STREQ(why_line("no.such.id"), "");
}

TEST(Overlay, EmptyEngineSweepsAndJsonIsFlatSorted) {
  GuideEngine e;
  auto m = e.overlay(123);
  EXPECT_EQ(m.mode, "sweep");
  EXPECT_EQ(m.why_id, "why.sweep");
  EXPECT_EQ(m.walls, 0);
  EXPECT_FALSE(m.wall_found || m.floor_found || m.ceiling_found);
  auto j = m.to_json();
  EXPECT_EQ(j.find('['), std::string::npos) << j;  // flat: no arrays
  EXPECT_NE(j.find("\"type\":\"overlay\""), std::string::npos);
  EXPECT_NE(j.find("\"surfaces\":\"\""), std::string::npos);
  EXPECT_EQ(j, e.overlay(123).to_json());
}

TEST(Overlay, TrackingAndWallBearingsAreRelativeToTheUser) {
  auto feed = [](GuideEngine& e, double yaw, int state, int64_t t0) {
    bundle::IntrinsicsSample k;
    k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 319.5, k.cy = 239.5;
    e.on_intrinsics(k);
    e.on_plane(wall("w", {2, 1.25, 4}, {0, 0, -1}, {1, 0, 0}, 4, 2.5));
    bundle::PointCloudRecord pc;  // sparse points on the wall: their sight rays free the floor in front of the user
    pc.t_ns = t0;
    for (double x = 0.5; x <= 3.5; x += 0.25)
      for (double y = 0.8; y <= 1.6; y += 0.2) pc.xyz.insert(pc.xyz.end(), {float(x), float(y), 4.f});
    for (int k = 0; k < 6; ++k) {
      e.on_pose(pose_at(t0 + k * 100'000'000, 2, 1, yaw, state));
      if (k == 0) e.on_points(pc);
    }
    std::vector<Instruction> out;
    e.update(t0 + 500'000'000, out);
  };
  GuideEngine lost;
  feed(lost, 0, 3, 1'000'000'000);
  EXPECT_EQ(lost.overlay(1'000'000'000).why_id, "why.tracking_lost");
  GuideEngine weak;
  feed(weak, 0, 2, 1'000'000'000);
  EXPECT_EQ(weak.overlay(1'000'000'000).why_id, "why.limited");

  // The unscanned wall is at +z. Facing +z it is ahead; yaw is positive to the left (+x), so at -90 deg it is on the left.
  GuideEngine a, l, r;
  feed(a, 0, 1, 1'000'000'000);
  feed(l, -kPi / 2, 1, 1'000'000'000);
  feed(r, kPi / 2, 1, 1'000'000'000);
  auto ma = a.overlay(2'000'000'000);
  ASSERT_EQ(ma.mode, "target") << ma.to_json();
  EXPECT_EQ(ma.walls, 1);  // v3: one layout wall with evidence; the outline's other sides are hypothesised (id < 0)
  EXPECT_TRUE(ma.wall_found);
  const OverlayWall* seen = nullptr;
  for (auto& w : ma.wall_list)
    if (w.id > 0) {
      ASSERT_EQ(seen, nullptr) << ma.to_json();
      seen = &w;
    }
  ASSERT_NE(seen, nullptr) << ma.to_json();
  EXPECT_EQ(seen->id, 1);
  EXPECT_EQ(seen->seen_pct, 0);
  EXPECT_NEAR(std::min(seen->x0, seen->x1), 0, 1e-6);
  EXPECT_NEAR(std::max(seen->x0, seen->x1), 4, 1e-6);
  EXPECT_NEAR(seen->z0, 4, 1e-6);
  EXPECT_NEAR(seen->z1, 4, 1e-6);
  EXPECT_EQ(ma.why_id, "why.wall_ahead");
  EXPECT_EQ(l.overlay(2'000'000'000).why_id, "why.wall_left");
  EXPECT_EQ(r.overlay(2'000'000'000).why_id, "why.wall_right");
}

TEST(Overlay, NonFiniteWallIsSkippedAndJsonStaysValid) {
  GuideEngine e;
  bundle::IntrinsicsSample k;
  k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 319.5, k.cy = 239.5;
  e.on_intrinsics(k);
  const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
  e.on_plane(wall("bad_nan", {2, 1.25, 4}, {0, 0, -1}, {1, 0, 0}, nan, 2.5));  // degenerate extent slips past LiveMap checks
  e.on_plane(wall("bad_inf", {2, 1.25, -4}, {0, 0, 1}, {1, 0, 0}, 4, inf));
  e.on_plane(wall("good", {-3, 1.25, 0}, {1, 0, 0}, {0, 0, 1}, 4, 2.5));
  e.on_pose(pose_at(1'000'000'000, 0, 0, 0));
  std::vector<Instruction> out;
  e.update(1'000'000'000, out);  // v3: the layout refreshes in update()
  auto m = e.overlay(1'000'000'000);
  auto j = m.to_json();
  for (const char* bad : {"nan", "NaN", "inf", "Inf"}) EXPECT_EQ(j.find(bad), std::string::npos) << bad << " in " << j;
  EXPECT_EQ(j.find('['), std::string::npos) << j;
  EXPECT_TRUE(m.wall_found);
  EXPECT_EQ(m.walls, 1);  // v3: non-finite planes never enter the layout; only the good wall is a layout wall
  ASSERT_FALSE(m.wall_list.empty()) << j;
  for (auto& w : m.wall_list) {
    EXPECT_TRUE(std::isfinite(w.x0 + w.z0 + w.x1 + w.z1));
    EXPECT_GE(w.seen_pct, 0);
    EXPECT_LE(w.seen_pct, 100);
  }
  // A hand-built model with non-finite endpoints and an out-of-range percentage is sanitised by to_json too.
  OverlayModel h;
  h.wall_list = {{nan, 0, 1, 1, 50}, {0, 0, 1, 1, 250}};
  auto hj = h.to_json();
  EXPECT_EQ(hj.find("nan"), std::string::npos) << hj;
  EXPECT_NE(hj.find("\"surfaces\":\"0.00,0.00,1.00,1.00,100,0\""), std::string::npos) << hj;
}

TEST(Overlay, FreshLineIdsArePrintedUnclamped) {
  // RoomShell's fresh line numbers start above 100000; the old +-9999 clamp printed them all as 9999 (collisions).
  OverlayModel h;
  h.wall_list = {{0, 0, 1, 1, 50, 100001}, {0, 0, 1, 1, 50, 100002}, {0, 0, 1, 1, 50, 9999999}};
  h.span_list = {{100001, 0, 0, 1, 1, 'o'}, {-100002, 0, 0, 1, 1, 'b'}};
  auto j = h.to_json();
  EXPECT_NE(j.find("\"surfaces\":\"0.00,0.00,1.00,1.00,50,100001;0.00,0.00,1.00,1.00,50,100002;0.00,0.00,1.00,1.00,50,999999\""),
            std::string::npos) << j;
  EXPECT_NE(j.find("\"spans\":\"100001,0.00,0.00,1.00,1.00,o;-100002,0.00,0.00,1.00,1.00,b\""), std::string::npos) << j;
}

// ADR 0022: yaw_mask = 12 x 30 degree heading sectors (bit 0 = [0,30), yaw positive to the left) that hold a keyframe,
// so room progress moves with zero planes.
TEST(Overlay, YawMaskFollowsKeyframeHeadingsWithoutPlanes) {
  GuideEngine e;
  bundle::IntrinsicsSample k;
  k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 319.5, k.cy = 239.5;
  e.on_intrinsics(k);
  EXPECT_EQ(e.overlay(0).yaw_mask, 0u);
  const double d2r = kPi / 180;
  const double yaws[] = {10 * d2r, 100 * d2r, -80 * d2r};  // sectors 0, 3, 9
  int64_t t = 1'000'000'000;
  for (double yaw : yaws) {
    for (int i = 0; i < 4; ++i) e.on_pose(pose_at(t + i * 100'000'000, 2, 1, yaw));
    e.on_keyframe(1, t + 150'000'000, 0.8);
    t += 1'000'000'000;
  }
  auto m = e.overlay(t);
  EXPECT_EQ(m.wall_list.size(), 0u);
  EXPECT_EQ(m.yaw_mask, (1u << 0) | (1u << 3) | (1u << 9));
  EXPECT_NE(m.to_json().find("\"yaw_mask\":" + std::to_string(m.yaw_mask)), std::string::npos) << m.to_json();
  // A pose without a keyframe adds nothing; the same heading again changes nothing.
  e.on_pose(pose_at(t, 2, 1, 200 * d2r));
  e.on_keyframe(2, t - 50'000'000, 0.8);  // interpolated at the previous heading (-80 deg)
  EXPECT_EQ(e.overlay(t).yaw_mask, m.yaw_mask);
  OverlayModel h;
  h.yaw_mask = 0x801;
  EXPECT_NE(h.to_json().find("\"yaw_mask\":2049"), std::string::npos) << h.to_json();
}

// ADR 0022: the wall id is the 6th surfaces field, 1-based in first-seen order, and never changes afterwards.
TEST(Overlay, WallIdsAreStableInFirstSeenOrder) {
  GuideEngine e;
  bundle::IntrinsicsSample k;
  k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 319.5, k.cy = 239.5;
  e.on_intrinsics(k);
  e.on_plane(wall("b", {2, 1.25, 4}, {0, 0, -1}, {1, 0, 0}, 4, 2.5));
  e.on_plane(wall("a", {-3, 1.25, 0}, {1, 0, 0}, {0, 0, 1}, 4, 2.5));
  e.on_pose(pose_at(1'000'000'000, 0, 0, 0));
  std::vector<Instruction> out;
  e.update(1'000'000'000, out);  // v3: walls are layout walls, refreshed in update()
  auto ids_of = [](const OverlayModel& m) {
    std::set<int> s;
    for (auto& w : m.wall_list)
      if (w.id > 0) s.insert(w.id);
    return s;
  };
  auto id_at_z4 = [](const OverlayModel& m) {  // the line of plane "b" (z = 4), seen first
    for (auto& w : m.wall_list)
      if (std::abs(w.z0 - 4) < 0.05 && std::abs(w.z1 - 4) < 0.05) return w.id;
    return 0;
  };
  auto m1 = e.overlay(1'000'000'000);
  EXPECT_EQ(ids_of(m1), (std::set<int>{1, 2})) << m1.to_json();  // ids are first-seen plane order: b = 1, a = 2
  EXPECT_EQ(id_at_z4(m1), 1);
  e.on_plane(wall("0", {3, 1.25, 0}, {-1, 0, 0}, {0, 0, 1}, 4, 2.5));  // sorts first by plane id, seen last
  e.on_pose(pose_at(2'000'000'000, 0, 0, 0));
  e.update(2'000'000'000, out);  // the layout refreshes at most once per event-second
  auto m2 = e.overlay(2'000'000'000);
  EXPECT_EQ(ids_of(m2), (std::set<int>{1, 2, 3})) << m2.to_json();
  EXPECT_EQ(id_at_z4(m2), 1);  // never renumbered
  auto j = m2.to_json();
  size_t p = j.find("\"surfaces\":\"");
  ASSERT_NE(p, std::string::npos);
  std::string s = j.substr(p + 12, j.find('"', p + 12) - p - 12);
  size_t entries = 0;
  for (size_t i = 0, n; i <= s.size(); i = n + 1) {
    n = s.find(';', i);
    if (n == std::string::npos) n = s.size();
    int commas = int(std::count(s.begin() + i, s.begin() + n, ','));
    EXPECT_EQ(commas, 5) << s;
    ++entries;
  }
  EXPECT_EQ(entries, m2.wall_list.size());
}

// Golden: generated on Linux x86_64 with g++ (Release, glibc libm trig). Coordinates are rounded to cm, but a different
// libm/arch may shift a value; regenerate on purpose with CK_UPDATE_GOLDEN=1 (see docs/guide.md) and review the diff.
TEST(Overlay, GoldenFromFixedSeedSimRun) {
  auto got = samples_of(golden_run());
  fs::path g = fs::path(CK_TEST_GOLDEN_DIR) / "overlay_box_seed1.jsonl";
  const char* upd = std::getenv("CK_UPDATE_GOLDEN");
  if (upd && *upd) {
    fs::create_directories(g.parent_path());
    std::ofstream(g, std::ios::binary) << got;
  }
  std::ifstream f(g, std::ios::binary);
  ASSERT_TRUE(f.good()) << "missing golden " << g << " (run with CK_UPDATE_GOLDEN=1)";
  std::stringstream ss;
  ss << f.rdbuf();
  std::string want = ss.str();
  want.erase(std::remove(want.begin(), want.end(), '\r'), want.end());  // git autocrlf on Windows
  EXPECT_EQ(got, want);
}

TEST(Overlay, SimRunIsByteIdenticalTwiceAndEventsStaySmall) {
  auto a = golden_run(), b = golden_run();
  EXPECT_EQ(a, b);
  sim::Config c;
  c.room = "furniture";
  c.policy = "planner";
  c.seed = 3;
  c.max_s = 45;
  c.overlay_every_s = 5;
  auto r = sim::simulate(c);
  auto p = r.find("\"overlay_max_bytes\":");
  ASSERT_NE(p, std::string::npos);
  size_t mx = std::stoul(r.substr(p + 20));
  EXPECT_GT(mx, 100u);
  EXPECT_LT(mx, 4096u) << "overlay event must stay under 4 KB";
  // Without the option the report carries no overlay fields (existing reports are unchanged).
  c.room = "box";
  c.max_s = 15;
  c.overlay_every_s = 0;
  EXPECT_EQ(sim::simulate(c).find("overlay_"), std::string::npos);
}
