// real-room-fix T1 (ADR 0026): the room layout model. A wall is a LINE built from plane fragments, bridged across
// furniture gaps when obstacles or wall points explain the gap, closed into an outline, with stable "W<n>" ids that
// survive plane re-ids / subsumption, plane-less walls inferred from points + floor edge, and o/b/m spans.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "capturekit/guide/engine.hpp"
#include "capturekit/guide/layout.hpp"
#include "capturekit/guide/room_shell.hpp"

using namespace capturekit;
using namespace capturekit::guide;

namespace {

// Box room x in [0, 5], z in [0, 4], walls 2.5 m high. Side S: z = 0, N: z = 4, W: x = 0, E: x = 5 (inward normals).
Vec3 side_n(char s) { return s == 'S' ? Vec3(0, 0, 1) : s == 'N' ? Vec3(0, 0, -1) : s == 'W' ? Vec3(1, 0, 0) : Vec3(-1, 0, 0); }
Vec3 side_u(char s) { return s == 'S' || s == 'N' ? Vec3(1, 0, 0) : Vec3(0, 0, 1); }
Vec3 side_at(char s, double a) {
  return s == 'S' ? Vec3(a, 0, 0) : s == 'N' ? Vec3(a, 0, 4) : s == 'W' ? Vec3(0, 0, a) : Vec3(5, 0, a);
}

/// A wall plane fragment covering along-coordinates [a0, a1] of side s, shifted `shift` m inward.
bundle::PlaneRecord frag(const std::string& id, char s, double a0, double a1, int64_t t_ns, double shift = 0) {
  bundle::PlaneRecord p;
  p.t_ns = t_ns, p.id = id, p.kind = "wall";
  Vec3 c = side_at(s, 0.5 * (a0 + a1)) + side_n(s) * shift + Vec3(0, 1.25, 0);
  Vec3 n = side_n(s), u = side_u(s);
  for (int i = 0; i < 3; ++i) p.center[i] = c[i], p.normal[i] = n[i], p.axis_u[i] = u[i];
  p.extent_u = a1 - a0, p.extent_v = 2.5;
  return p;
}

bundle::PlaneRecord floor_rec(int64_t t_ns) {
  bundle::PlaneRecord p;
  p.t_ns = t_ns, p.id = "floor", p.kind = "floor";
  p.center[0] = 2.5, p.center[1] = 0, p.center[2] = 2;
  p.extent_u = 5, p.extent_v = 4;
  return p;
}

/// Furniture: point obstacles filling a box footprint (two hits per NavGrid cell) up to height h.
void furniture(NavGrid& nav, SparsePointStore& pts, double x0, double x1, double z0, double z1, double h) {
  for (double x = x0; x <= x1 + 1e-9; x += 0.05)
    for (double z = z0; z <= z1 + 1e-9; z += 0.05)
      for (double y : {0.3, h}) {
        nav.add_point({x, y, z});
        nav.add_point({x, y, z});
        pts.add({x, y, z});
      }
}

/// Engine-like wall numbers (GuideEngine::assign_wall_ids): first-seen order over the map, never reused.
struct Numbers {
  RoomShell::WallIds ids;
  int next = 1;
  void assign(const LiveMap& m) {
    for (auto& [id, pl] : m.planes())
      if (pl.kind == "wall" && !ids.count(id)) ids[id] = next++;
  }
};

int lines_with_k(const RoomShell& sh, int k) {
  return int(std::count_if(sh.lines().begin(), sh.lines().end(), [&](const LayoutLine& l) { return l.k == k; }));
}

/// Index of the shell wall whose midpoint is closest to world point (x, z).
size_t wall_near(const ShellModel& s, double x, double z) {
  size_t best = 0;
  double bd = 1e18;
  for (size_t i = 0; i < s.walls.size(); ++i) {
    auto& w = s.walls[i];
    double d = std::hypot(0.5 * (w.x0 + w.x1) - x, 0.5 * (w.z0 + w.z1) - z);
    if (d < bd) bd = d, best = i;
  }
  return best;
}

}  // namespace

TEST(Layout, WallSplitBySofaIsOneLine) {
  LiveMap m;
  m.add_plane(frag("s0", 'S', 0.0, 1.6, 1));
  m.add_plane(frag("s1", 'S', 3.1, 5.0, 1));  // 1.5 m hidden by a sofa
  m.add_plane(frag("e", 'E', 0, 4, 1));
  m.add_plane(frag("n", 'N', 0, 5, 1));
  m.add_plane(frag("w", 'W', 0, 4, 1));
  Numbers num;
  num.assign(m);
  GuideParams gp;
  NavGrid nav(gp);
  SparsePointStore pts;

  RoomShell bare;  // planes only: a 1.5 m gap is not bridged
  bare.update(m, &num.ids);
  EXPECT_EQ(bare.lines().size(), 5u);

  furniture(nav, pts, 1.5, 3.2, 0.1, 0.9, 0.8);
  LayoutEvidence ev{&pts, &nav};
  RoomShell sh;
  sh.update(m, &num.ids, {}, &ev);
  ASSERT_EQ(sh.lines().size(), 4u);
  const ShellModel& s = sh.shell();
  ASSERT_EQ(s.walls.size(), 4u);
  EXPECT_TRUE(s.closed);
  EXPECT_FALSE(s.low_confidence);  // a chained 4-line loop, not the bounding-rectangle fallback
  EXPECT_EQ(sh.evidenced_walls(), 4);
  EXPECT_TRUE(sh.closed_with_evidence());
  size_t si = wall_near(s, 2.5, 0);
  ASSERT_EQ(sh.layout_walls().size(), 4u);
  auto planes = sh.layout_walls()[si].planes;
  std::sort(planes.begin(), planes.end());
  EXPECT_EQ(planes, (std::vector<std::string>{"s0", "s1"}));
  EXPECT_EQ(s.walls[si].id, "W" + std::to_string(std::min(num.ids["s0"], num.ids["s1"])));

  // Spans of the sofa wall: observed, blocked behind the sofa, observed.
  double blocked = 0, observed = 0;
  for (auto& sp : sh.spans()) {
    if (sp.wall != s.walls[si].id) continue;
    EXPECT_NEAR(sp.z0, 0.0, 1e-6);
    if (sp.state == 'b') {
      blocked += sp.length();
      EXPECT_NEAR(0.5 * (sp.x0 + sp.x1), 2.35, 0.2);
    } else if (sp.state == 'o') {
      observed += sp.length();
    }
  }
  EXPECT_NEAR(blocked, 1.5, 0.25);
  EXPECT_NEAR(observed, 3.5, 0.25);
  EXPECT_NEAR(sh.blocked_m(), 1.5, 0.25);
}

TEST(Layout, ParallelWallsAndOppositeFacesDoNotMerge) {
  for (double depth : {0.2, 0.5}) {  // an alcove / jog: parallel wall pieces 0.2-0.5 m apart stay two lines
    LiveMap m;
    m.add_plane(frag("s0", 'S', 0.0, 2.0, 1));
    m.add_plane(frag("s1", 'S', 2.3, 5.0, 1, -depth));  // recessed (outward) by depth
    m.add_plane(frag("e", 'E', 0, 4, 1));
    m.add_plane(frag("n", 'N', 0, 5, 1));
    m.add_plane(frag("w", 'W', 0, 4, 1));
    GuideParams gp;
    NavGrid nav(gp);
    SparsePointStore pts;
    furniture(nav, pts, 1.5, 3.0, 0.1, 0.9, 0.8);
    LayoutEvidence ev{&pts, &nav};
    RoomShell sh;
    sh.update(m, nullptr, {}, &ev);
    EXPECT_EQ(lines_with_k(sh, 1), 2) << "depth " << depth;  // k 1 = inward normal +z'
  }
  // A thin partition: the two faces 5 cm apart have opposite normals and are never one line.
  LiveMap m;
  m.add_plane(frag("e", 'E', 0, 4, 1));
  m.add_plane(frag("w", 'W', 0, 4, 1));
  auto a = frag("pa", 'S', 1, 4, 1, 2.0);   // z = 2.0, normal +z
  auto b = frag("pb", 'N', 1, 4, 1, 1.95);  // z = 2.05, normal -z
  m.add_plane(a);
  m.add_plane(b);
  RoomShell sh;
  sh.update(m);
  int horiz = 0;
  for (auto& l : sh.lines()) horiz += l.axis == 1;
  EXPECT_EQ(horiz, 2);
}

namespace {
struct FragRun {
  std::vector<std::vector<std::string>> ids_per_step;  // shell wall ids (sorted by line) after each step
  std::vector<int> walls_per_step;
  std::vector<int> chained_per_step;  // 1 = a closed 4-line loop (not the bounding-rectangle fallback)
  std::vector<std::string> final_planes;
};

/// 4-wall room seen as 9 fragments ("a*", t = 1..3 s), then re-detected as new ids ("b*", shifted 0.12 m inward and
/// grown, t = 4..7 s) while the old ids stop updating, as ARCore does when it re-ids / subsumes planes.
FragRun run_fragments() {
  struct F {
    char s;
    double a0, a1;
  };
  const std::vector<F> fr = {{'S', 0.0, 1.2}, {'S', 1.7, 2.4}, {'S', 3.8, 5.0}, {'E', 0.0, 1.8}, {'E', 2.4, 4.0},
                             {'N', 0.0, 2.0}, {'N', 3.2, 5.0}, {'W', 0.0, 1.6}, {'W', 2.0, 4.0}};
  LiveMap m;
  m.add_camera({2, 1.4, 2}), m.add_camera({3, 1.4, 1.5});
  GuideParams gp;
  NavGrid nav(gp);
  SparsePointStore pts;
  furniture(nav, pts, 2.4, 3.8, 0.1, 0.9, 0.8);  // sofa in the S gap (1.4 m)
  for (double x = 2.0; x <= 3.2 + 1e-9; x += 0.05)  // N gap (1.2 m): wall points above a low cabinet
    for (double y : {1.3, 1.7, 2.1}) pts.add({x, y, 3.98});
  LayoutEvidence ev{&pts, &nav};
  Numbers num;
  RoomShell sh;
  FragRun out;
  for (int t = 1; t <= 7; ++t) {
    const int64_t tn = int64_t(t) * 1'000'000'000;
    for (size_t i = 0; i < fr.size(); ++i) {
      const F& f = fr[i];
      double L = f.s == 'S' || f.s == 'N' ? 5.0 : 4.0;
      if (t <= 3) m.add_plane(frag("a" + std::to_string(i), f.s, f.a0, f.a1, tn));
      else m.add_plane(frag("b" + std::to_string(i), f.s, std::max(0.0, f.a0 - 0.2), std::min(L, f.a1 + 0.2), tn, 0.12));
    }
    num.assign(m);
    sh.update(m, &num.ids, {}, &ev);
    std::vector<std::string> ids;
    for (char s : {'S', 'E', 'N', 'W'}) {
      Vec3 c = side_at(s, s == 'S' || s == 'N' ? 2.5 : 2.0);
      ids.push_back(sh.shell().walls.empty() ? "" : sh.shell().walls[wall_near(sh.shell(), c.x(), c.z())].id);
    }
    out.ids_per_step.push_back(ids);
    out.walls_per_step.push_back(sh.evidenced_walls());
    out.chained_per_step.push_back(sh.shell().closed && !sh.shell().low_confidence);
  }
  for (auto& [id, _] : m.planes()) out.final_planes.push_back(id);
  return out;
}
}  // namespace

TEST(Layout, FragmentsAndReIdsGiveFourWallsWithStableIds) {
  FragRun r = run_fragments();
  ASSERT_EQ(r.ids_per_step.size(), 7u);
  // First-seen numbers in map order: a0..a8 -> 1..9, so the lines are S W1, E W4, N W6, W W8.
  const std::vector<std::string> want = {"W1", "W4", "W6", "W8"};
  for (size_t t = 0; t < r.ids_per_step.size(); ++t) {
    EXPECT_EQ(r.walls_per_step[t], 4) << "step " << t + 1;
    EXPECT_EQ(r.chained_per_step[t], 1) << "step " << t + 1;
    EXPECT_EQ(r.ids_per_step[t], want) << "step " << t + 1;
  }
  for (auto& id : r.final_planes) EXPECT_NE(id[0], 'a') << id << " should have been aliased (subsumed)";
}

// Review round 1: a parallel alcove line 0.2 m behind the front wall must not take the front wall's id once every
// front-wall plane has been re-detected under a new id (the old track is nearer to the front line).
TEST(Layout, AlcoveDoesNotStealIdAfterFrontWallReId) {
  LiveMap m;
  m.add_camera({2, 1.4, 2}), m.add_camera({3, 1.4, 1.5});
  Numbers num;
  RoomShell sh;
  auto line_ids = [&]() {  // inward +z lines (k 1) by offset: {front, alcove}
    std::string front, alcove;
    for (auto& l : sh.lines())
      if (l.k == 1) (l.off > -0.12 ? front : alcove) = l.id;
    return std::make_pair(front, alcove);
  };
  std::pair<std::string, std::string> first;
  for (int t = 1; t <= 7; ++t) {
    const int64_t tn = int64_t(t) * 1'000'000'000;
    if (t <= 3) m.add_plane(frag("f", 'S', 0.0, 2.0, tn));
    else m.add_plane(frag("h", 'S', 0.0, 2.1, tn, 0.12));  // the front wall re-id'd: new id, 12 cm off, grown
    m.add_plane(frag("g", 'S', 2.3, 5.0, tn, -0.2));       // alcove recess, same facing, 0.2 m behind (within track_off)
    m.add_plane(frag("e", 'E', 0, 4, tn));
    m.add_plane(frag("n", 'N', 0, 5, tn));
    m.add_plane(frag("w", 'W', 0, 4, tn));
    num.assign(m);
    sh.update(m, &num.ids);
    auto ids = line_ids();
    if (t == 1) {
      first = ids;
      EXPECT_EQ(first.first, "W" + std::to_string(num.ids["f"]));
      EXPECT_EQ(first.second, "W" + std::to_string(num.ids["g"]));
    }
    EXPECT_EQ(ids, first) << "step " << t;
  }
  EXPECT_EQ(m.planes().count("f"), 0u);  // subsumed by "h": the front line has only the new plane now
}

TEST(Layout, MergeDoesNotDriftIntoAlcove) {
  // Wall at 0.00 + a heavier fragment at 0.12 (weighted mean ~0.10): an alcove at 0.24 must not chain onto the mean.
  LiveMap m;
  m.add_plane(frag("s0", 'S', 0.0, 0.6, 1));
  m.add_plane(frag("s1", 'S', 0.8, 3.8, 1, 0.12));
  m.add_plane(frag("s2", 'S', 4.0, 5.0, 1, 0.24));
  m.add_plane(frag("e", 'E', 0, 4, 1));
  m.add_plane(frag("n", 'N', 0, 5, 1));
  m.add_plane(frag("w", 'W', 0, 4, 1));
  ASSERT_EQ(m.planes().size(), 6u);
  RoomShell sh;
  sh.update(m);
  EXPECT_EQ(lines_with_k(sh, 1), 2);
  for (auto& l : sh.lines()) {
    auto has = [&](const char* p) { return std::find(l.planes.begin(), l.planes.end(), p) != l.planes.end(); };
    EXPECT_FALSE(has("s0") && has("s2")) << l.id << " spans 0.24 m of offset";
  }
}

TEST(Layout, IsDeterministic) {
  FragRun a = run_fragments(), b = run_fragments();
  EXPECT_EQ(a.ids_per_step, b.ids_per_step);
  EXPECT_EQ(a.walls_per_step, b.walls_per_step);
  EXPECT_EQ(a.final_planes, b.final_planes);
}

TEST(Layout, SubsumedPlaneIsAliased) {
  LiveMap m;
  m.add_plane(frag("P", 'S', 1, 3, 1'000'000'000));
  m.add_plane(frag("Q", 'S', 0, 5, 1'000'000'000, 0.12));  // 12 cm off: not merged as a duplicate on arrival
  m.add_plane(frag("R", 'S', 6.5, 7.5, 1'000'000'000));    // stale too, but nothing covers it
  ASSERT_EQ(m.planes().size(), 3u);
  m.add_plane(frag("Q", 'S', 0, 5, 2'000'000'000, 0.12));
  EXPECT_EQ(m.planes().count("P"), 1u) << "1 s stale is not yet subsumed";
  m.add_plane(frag("Q", 'S', 0, 5, 3'000'000'000, 0.12));
  EXPECT_EQ(m.planes().count("P"), 0u);
  EXPECT_EQ(m.canonical("P"), "Q");
  EXPECT_EQ(m.planes().count("R"), 1u);
  // A late record of the subsumed id must not shrink the subsumer to its own small rectangle.
  m.add_plane(frag("P", 'S', 1, 3, 4'000'000'000));
  ASSERT_EQ(m.planes().count("Q"), 1u);
  EXPECT_GE(m.planes().at("Q").eu, 5.0 - 1e-9);
  EXPECT_EQ(m.planes().count("P"), 0u);
}

TEST(Layout, SubsumedPlaneGrowthIsKept) {
  LiveMap m;
  m.add_plane(frag("P", 'S', 1, 3, 1'000'000'000));
  m.add_plane(frag("Q", 'S', 0, 4, 1'000'000'000, 0.12));
  m.add_plane(frag("Q", 'S', 0, 4, 3'000'000'000, 0.12));
  ASSERT_EQ(m.planes().count("P"), 0u);
  ASSERT_EQ(m.canonical("P"), "Q");
  // P grows past Q's end: the growth is united into Q (not dropped), and P's later records keep counting.
  m.add_plane(frag("P", 'S', 1, 5.5, 4'000'000'000));
  ASSERT_EQ(m.planes().count("Q"), 1u);
  EXPECT_EQ(m.planes().count("P"), 0u);
  EXPECT_EQ(m.canonical("P"), "Q");
  EXPECT_NEAR(m.planes().at("Q").eu, 5.5, 1e-6);
  m.add_plane(frag("P", 'S', 1, 6.0, 5'000'000'000));
  EXPECT_NEAR(m.planes().at("Q").eu, 6.0, 1e-6);
  // A subsumed plane that turned (no longer parallel) is a plane of its own again.
  LiveMap m2;
  m2.add_plane(frag("P", 'S', 1, 3, 1'000'000'000));
  m2.add_plane(frag("Q", 'S', 0, 4, 3'000'000'000, 0.12));
  m2.add_plane(frag("Q", 'S', 0, 4, 3'100'000'000, 0.12));
  ASSERT_EQ(m2.planes().count("P"), 0u);
  m2.add_plane(frag("P", 'E', 0, 4, 4'000'000'000));
  EXPECT_EQ(m2.planes().count("P"), 1u);
  EXPECT_EQ(m2.canonical("P"), "P");
}

TEST(Layout, TexturelessWallInferredFromFloorEdgeAndPoints) {
  for (bool with_points : {false, true}) {
    LiveMap m;
    m.add_camera({2, 1.4, 2}), m.add_camera({3, 1.4, 1}), m.add_camera({4, 1.4, 3});
    m.add_plane(floor_rec(1));
    m.add_plane(frag("s", 'S', 1.2, 5.0, 1));  // furniture hides the W ends of S and N
    m.add_plane(frag("e", 'E', 0, 4, 1));
    m.add_plane(frag("n", 'N', 1.5, 5.0, 1));  // no plane at all on the textureless W wall (x = 0)
    GuideParams gp;
    NavGrid nav(gp);
    SparsePointStore pts;
    if (with_points)
      for (double z = 0.5; z <= 3.5 + 1e-9; z += 0.25)
        for (double y : {0.5, 1.5}) pts.add({0.02, y, z});  // a socket, a picture edge, the skirting ...
    LayoutEvidence ev{&pts, &nav};
    Numbers num;
    num.assign(m);
    RoomShell sh;
    sh.update(m, &num.ids, {}, &ev);
    const ShellModel& s = sh.shell();
    ASSERT_EQ(s.walls.size(), 4u) << with_points;
    size_t wi = wall_near(s, 0.5, 2.0);
    EXPECT_TRUE(s.walls[wi].inferred);
    EXPECT_EQ(s.walls[wi].id[0], 'I');
    if (with_points) {
      EXPECT_EQ(sh.evidenced_walls(), 4);
      EXPECT_TRUE(sh.layout_walls()[wi].evidenced);
      EXPECT_TRUE(sh.closed_with_evidence());
      EXPECT_NEAR(s.walls[wi].x0, 0.0, 0.15);
      EXPECT_NEAR(s.walls[wi].x1, 0.0, 0.15);
      EXPECT_LT(sh.layout_walls()[wi].num, 0);
    } else {
      EXPECT_EQ(sh.evidenced_walls(), 3);
      EXPECT_FALSE(sh.layout_walls()[wi].evidenced);
      EXPECT_FALSE(sh.closed_with_evidence());
      EXPECT_NEAR(s.walls[wi].x0, 1.2, 0.15);  // the hypothesised side sits at the observed extent
    }
  }
}

TEST(Layout, WallNumbersFromIds) {
  EXPECT_EQ(layout_wall_num("W3"), 3);
  EXPECT_EQ(layout_wall_num("W12"), 12);
  EXPECT_EQ(layout_wall_num("I2"), -2);
  EXPECT_EQ(layout_wall_num(""), 0);
  EXPECT_EQ(layout_wall_num("X1"), 0);
}

namespace {
bundle::PoseSample pose(int64_t t, double x, double z, double yaw, double pitch = 0) {
  auto T = pose_from_yaw_pitch({x, 1.4, z}, yaw, pitch);
  bundle::PoseSample p;
  p.t_ns = t;
  for (int i = 0; i < 3; ++i) p.t_world_from_camera[i] = T.translation()[i];
  auto q = T.se3().unit_quaternion();
  p.q_world_from_camera_wxyz[0] = q.w(), p.q_world_from_camera_wxyz[1] = q.x();
  p.q_world_from_camera_wxyz[2] = q.y(), p.q_world_from_camera_wxyz[3] = q.z();
  p.tracking_state = static_cast<bundle::TrackingState>(1);
  return p;
}
std::string key(const std::string& j, const std::string& k) {
  auto p = j.find("\"" + k + "\":");
  if (p == std::string::npos) return "<absent>";
  p += k.size() + 3;
  if (j[p] == '"') return j.substr(p + 1, j.find('"', p + 1) - p - 1);
  return j.substr(p, j.find_first_of(",}", p) - p);
}
}  // namespace

TEST(Layout, EngineOverlayV3AndReportKeys) {
  GuideEngine e;  // default params: understanding 0, the layout still runs
  EXPECT_EQ(e.params().version, "gp2");
  bundle::IntrinsicsSample k;
  k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 320, k.cy = 240;
  e.on_intrinsics(k);
  std::vector<Instruction> out;
  int64_t t = 1'000'000'000;
  e.on_pose(pose(t, 2.5, 2, 0));
  e.update(t, out);
  {
    auto j = e.overlay(t).to_json();  // nothing yet: v3 keys are present but empty
    EXPECT_EQ(key(j, "walls"), "0");
    EXPECT_EQ(key(j, "spans"), "");
    EXPECT_EQ(key(j, "shell"), "");
    EXPECT_EQ(key(j, "shell_closed"), "false");
    EXPECT_EQ(key(j, "ceiling_seen"), "false");
  }
  for (auto r : {frag("s0", 'S', 0, 1.6, t), frag("s1", 'S', 3.1, 5, t), frag("e", 'E', 0, 4, t), frag("n", 'N', 0, 5, t),
                 frag("w", 'W', 0, 4, t)})
    e.on_plane(r);
  bundle::PointCloudRecord pc;
  pc.t_ns = t;
  for (double x = 1.5; x <= 3.2; x += 0.05)
    for (double z = 0.1; z <= 0.9; z += 0.05)
      for (double y : {0.3, 0.8, 0.31, 0.81}) pc.xyz.insert(pc.xyz.end(), {float(x), float(y), float(z)});
  e.on_points(pc);
  t += 1'000'000'000;
  e.on_pose(pose(t, 2.5, 2, 0));
  e.update(t, out);
  auto m = e.overlay(t);
  EXPECT_EQ(m.walls, 4);  // five wall planes, four layout walls
  ASSERT_EQ(m.wall_list.size(), 4u);
  EXPECT_TRUE(m.shell_closed);
  EXPECT_EQ(m.shell_corners.size(), 4u);
  bool any_blocked = false;
  for (auto& sp : m.span_list) any_blocked |= sp.state == 'b';
  EXPECT_TRUE(any_blocked);
  auto j = m.to_json();
  EXPECT_EQ(key(j, "walls"), "4");
  EXPECT_EQ(key(j, "shell_closed"), "true");
  EXPECT_NE(key(j, "spans").find(",b"), std::string::npos) << j;
  const std::string surf = key(j, "surfaces");
  EXPECT_EQ(std::count(surf.begin(), surf.end(), ';'), 3) << surf;
  // Looking up at the (planeless) ceiling counts as seen.
  t += 100'000'000;
  e.on_pose(pose(t, 2.5, 2, 0, deg2rad(35)));
  e.on_keyframe(1, t, 1.0);
  EXPECT_TRUE(e.overlay(t).ceiling_seen);
  for (int i = 0; i < 12; ++i) {
    t += 1'000'000'000;
    e.on_pose(pose(t, 2.5, 2, 0.3 * i));
    e.update(t, out);
  }
  auto rep = e.report_json();
  auto lp = rep.find("\"layout\":{");
  ASSERT_NE(lp, std::string::npos) << rep;
  auto lj = rep.substr(lp, rep.find('}', lp) - lp + 1);
  EXPECT_EQ(key(lj, "walls"), "4");
  EXPECT_EQ(key(lj, "closed"), "true");
  EXPECT_GT(std::stod(key(lj, "blocked_m")), 1.0);
  auto tp = rep.find("\"timeline\":[{");
  ASSERT_NE(tp, std::string::npos) << rep;
  auto tl = rep.substr(tp);
  EXPECT_NE(tl.find("\"t_s\":0"), std::string::npos);
  EXPECT_NE(tl.find("\"prompts\":"), std::string::npos);
  EXPECT_NE(tl.find("\"walls\":4"), std::string::npos);
}

TEST(Layout, LimitSpansIsFairPerWall) {
  // Six walls of six spans each (o b o m o b ...): the 24-span cap used to cut the last two walls entirely.
  std::vector<LayoutSpan> in;
  const char st[6] = {'o', 'b', 'o', 'm', 'o', 'b'};
  for (int w = 1; w <= 6; ++w)
    for (int i = 0; i < 6; ++i) in.push_back({"W" + std::to_string(w), double(i), double(w), double(i + 1), double(w), st[i]});
  auto out = limit_spans(in, 24);
  ASSERT_LE(out.size(), 24u);
  for (int w = 1; w <= 6; ++w) {
    const std::string id = "W" + std::to_string(w);
    double x0 = 1e9, x1 = -1e9;
    int n = 0, blocked = 0;
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i].wall != id) continue;
      ++n, blocked += out[i].state == 'b';
      x0 = std::min(x0, out[i].x0), x1 = std::max(x1, out[i].x1);
      if (i > 0 && out[i - 1].wall == id) EXPECT_DOUBLE_EQ(out[i - 1].x1, out[i].x0) << id << ": spans stay contiguous";
    }
    EXPECT_GE(n, 2) << id;
    EXPECT_GE(blocked, 1) << id << " lost its blocked span";
    EXPECT_DOUBLE_EQ(x0, 0.0) << id;
    EXPECT_DOUBLE_EQ(x1, 6.0) << id;
  }
  EXPECT_EQ(limit_spans(in, 100).size(), in.size());  // under the cap: unchanged
}

TEST(Layout, ReportRecomputesLayoutWhenOnlyPointsChanged) {
  GuideEngine e;
  bundle::IntrinsicsSample k;
  k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 320, k.cy = 240;
  e.on_intrinsics(k);
  std::vector<Instruction> out;
  int64_t t = 1'000'000'000;
  for (auto [x, z] : std::vector<std::pair<double, double>>{{2, 2}, {3, 1}, {4, 3}}) e.on_pose(pose(t += 100'000'000, x, z, 0));
  for (auto r : {floor_rec(t), frag("s", 'S', 1.2, 5.0, t), frag("e", 'E', 0, 4, t), frag("n", 'N', 1.5, 5.0, t)}) e.on_plane(r);
  e.update(t, out);
  auto walls = [&] {
    auto rep = e.report_json();
    auto lp = rep.find("\"layout\":{");
    return lp == std::string::npos ? std::string("<absent>") : key(rep.substr(lp, rep.find('}', lp) - lp + 1), "walls");
  };
  EXPECT_EQ(walls(), "3");
  // Points on the textureless W wall arrive within the same event-second (the throttled layout is not refreshed) and
  // the map does not change: the final report must still see the inferred wall.
  bundle::PointCloudRecord pc;
  pc.t_ns = t;
  for (double z = 0.5; z <= 3.5 + 1e-9; z += 0.25)
    for (double y : {0.5, 1.5}) pc.xyz.insert(pc.xyz.end(), {0.02f, float(y), float(z)});
  e.on_points(pc);
  EXPECT_EQ(walls(), "4");
}
