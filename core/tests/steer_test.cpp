// Phase 9 step 8: gap steering behind GuideParams::understanding >= 2 (planner hop / clearance filters and gap views,
// gap instructions, overlay keys, engine-owned shell + gaps) and the flag 0 / 1 / 2 guide-eval comparison.
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <thread>

#include "capturekit/guide/engine.hpp"
#include "capturekit/guide/sim.hpp"
#include "capturekit/util/json.hpp"

using namespace capturekit;
using namespace capturekit::guide;

namespace {

geometry::Intrinsics K640() {
  geometry::Intrinsics K;
  K.width = 640, K.height = 480, K.fx = K.fy = 500, K.cx = 319.5, K.cy = 239.5;
  return K;
}

bundle::PlaneRecord plane(const std::string& id, const std::string& kind, Vec3 c, Vec3 n, Vec3 u, double eu, double ev) {
  bundle::PlaneRecord p;
  p.id = id, p.kind = kind;
  for (int i = 0; i < 3; ++i) p.center[i] = c[i], p.normal[i] = n[i], p.axis_u[i] = u[i];
  p.extent_u = eu, p.extent_v = ev;
  return p;
}

bundle::PlaneRecord wall_plane(const std::string& id, double x0, double z0, double x1, double z1) {
  const double L = std::hypot(x1 - x0, z1 - z0);
  const Vec3 u((x1 - x0) / L, 0, (z1 - z0) / L), n(-u.z(), 0, u.x());
  return plane(id, "wall", Vec3(0.5 * (x0 + x1), 1.25, 0.5 * (z0 + z1)), n, u, L, 2.5);
}

int words(const std::string& t) {
  std::istringstream is(t);
  int n = 0;
  for (std::string w; is >> w;) ++n;
  return n;
}

// ---- planner scene: a 4 x 4 m room, back wall only, fully scanned (completeness >= 0.9) ----
struct Room {
  GuideParams p;
  LiveMap map;
  Coverage cov{GuideParams{}};
  NavGrid nav{GuideParams{}};
  Planner planner{GuideParams{}};
  geometry::Intrinsics K = K640();
  UserState user;
  explicit Room(bool steer, bool scanned = true) {
    nav.set_floor_y(0);
    map.add_plane(plane("b", "wall", {2, 1.25, 4}, {0, 0, -1}, {1, 0, 0}, 4, 2.5));
    cov.sync(map);
    if (scanned)
      for (double x = 0.6; x <= 3.5; x += 0.3)
        for (double z : {0.5, 0.8, 1.1, 1.4})
          for (double yaw : {-0.3, 0.0, 0.3})
            for (double pitch : {-0.25, 0.0, 0.25}) cov.observe(Obs{pose_from_yaw_pitch({x, 1.4, z}, yaw, pitch), K, 0, 1.0, true}, nullptr, nullptr);
    for (int i = 0; i < 40; ++i)
      for (double h : {0.4, 1.2, 2.3})
        for (int k = 0; k < 2; ++k) {
          nav.add_point({0.0, h, 0.1 * i});
          nav.add_point({0.1 * i, h, 4.0});
        }
    for (int ix = 3; ix < 40; ++ix)
      for (int iz = 3; iz < 37; ++iz) nav.mark_open({(ix + 0.5) * 0.1, 0, (iz + 0.5) * 0.1});
    user.pos = {2, 1.4, 1.0};
    user.yaw = 0;
    nav.mark_traversed(user.pos);
    nav.rebuild();
    if (steer) planner.set_steer(SteerParams{});
  }
};

GapTarget wall_gap(double x, double z, const char* why = "the door wall") {
  GapTarget g;
  g.type = int(GapType::MissingWall), g.x = x, g.z = z, g.y = 1.0, g.len = 3.0, g.pri = 0.9, g.why = why;
  return g;
}

constexpr int64_t S = 1'000'000'000LL;

}  // namespace

TEST(Steer, DefaultParamsAndFlagGate) {
  GuideParams p;
  EXPECT_EQ(p.understanding, 0);
  EXPECT_FALSE(Planner(p).steering());
  SteerParams sp;
  EXPECT_DOUBLE_EQ(sp.min_hop_m, 1.0);
  EXPECT_DOUBLE_EQ(sp.unseen_clear_m, 0.6);
  EXPECT_DOUBLE_EQ(sp.gap_gain, 200);
  EXPECT_DOUBLE_EQ(sp.gap_dist_min, 1.2);
  EXPECT_DOUBLE_EQ(sp.gap_dist_max, 3.5);
  EXPECT_EQ(sp.max_gap_visits, 2);
  EXPECT_DOUBLE_EQ(sp.gap_match_m, 0.6);
  EXPECT_DOUBLE_EQ(sp.gap_after, 0.6);
  for (double u : {0.0, 1.0}) {
    GuideParams q;
    q.understanding = u;
    EXPECT_FALSE(GuideEngine(q).planner().steering()) << u;
  }
  GuideParams q;
  q.understanding = 2;
  EXPECT_TRUE(GuideEngine(q).planner().steering());
  // The flag value 2 changes no config hash field other than itself; 0 and 1 are the Phase 8 / step-5 hashes.
  EXPECT_NE(q.hash(), GuideParams{}.hash());
}

TEST(Steer, FlagOffIgnoresGapsAndFinishes) {
  Room r(false);
  ASSERT_GE(r.cov.stats().completeness(), 0.9) << "the scene must be fully scanned";
  PlanHints h;
  h.gaps.push_back(wall_gap(2.0, 3.9));
  auto plan = r.planner.replan(0, r.user, r.cov, r.nav, r.K, 1.4, h);
  EXPECT_EQ(plan.mode, Mode::Finish);
  EXPECT_FALSE(plan.target.gap);
}

TEST(Steer, OpenGapDefersFinishThenFinishesAfterTwoVisits) {
  Room r(true);
  ASSERT_GE(r.cov.stats().completeness(), 0.9);
  PlanHints none;
  EXPECT_EQ(r.planner.replan(0, r.user, r.cov, r.nav, r.K, 1.4, none).mode, Mode::Finish) << "no gap: finish as before";
  PlanHints h;
  h.gaps.push_back(wall_gap(2.0, 3.9));
  auto p1 = r.planner.replan(S, r.user, r.cov, r.nav, r.K, 1.4, h);
  ASSERT_EQ(p1.mode, Mode::Target) << "an open gap defers Finish";
  ASSERT_TRUE(p1.target.valid);
  EXPECT_TRUE(p1.target.gap);
  EXPECT_EQ(p1.target.gap_type, int(GapType::MissingWall));
  EXPECT_DOUBLE_EQ(p1.target.gap_x, 2.0);
  EXPECT_DOUBLE_EQ(p1.target.gap_z, 3.9);
  EXPECT_EQ(p1.target.why, "the door wall");
  // The viewpoint sees the gap from the band and faces it.
  const double d = std::hypot(p1.target.x - 2.0, p1.target.z - 3.9);
  EXPECT_GE(d, 1.2 - 1e-9);
  EXPECT_LE(d, 3.5 + 1e-9);
  EXPECT_NEAR(wrap_pi(p1.target.yaw - std::atan2(2.0 - p1.target.x, 3.9 - p1.target.z)), 0.0, 1e-9);
  int ix, iz;
  r.nav.to_cell({p1.target.x, 0, p1.target.z}, ix, iz);
  EXPECT_TRUE(r.nav.passable(ix, iz));
  EXPECT_GE(r.nav.clearance(ix, iz), 0.6);
  EXPECT_EQ(r.planner.gap_picks(), 1);
  // Stand at the viewpoint and look: two dwelled visits use the gap up, then the planner finishes.
  UserState at = r.user;
  at.pos = {p1.target.x, 1.4, p1.target.z};
  at.yaw = p1.target.yaw;
  int64_t t = 2 * S;
  int guard = 0;
  Mode m = Mode::Target;
  while (m == Mode::Target && guard++ < 60) {
    auto pl = r.planner.replan(t, at, r.cov, r.nav, r.K, 1.4, h);
    m = pl.mode;
    if (pl.target.valid) {
      EXPECT_TRUE(pl.target.gap);
      at.pos = {pl.target.x, 1.4, pl.target.z}, at.yaw = pl.target.yaw;
    }
    t += S / 2;
  }
  EXPECT_EQ(m, Mode::Finish) << "after max_gap_visits the gap is given up on";
  EXPECT_EQ(r.planner.reached_count(), 2);
  EXPECT_LT(guard, 60);
}

TEST(Steer, ClosedGapEndsTheTargetAndUnreachableGapFinishes) {
  Room r(true);
  PlanHints h;
  h.gaps.push_back(wall_gap(2.0, 3.9));
  ASSERT_EQ(r.planner.replan(0, r.user, r.cov, r.nav, r.K, 1.4, h).mode, Mode::Target);
  PlanHints closed;  // the user looked there: the gap is gone from the finder
  EXPECT_EQ(r.planner.replan(S, r.user, r.cov, r.nav, r.K, 1.4, closed).mode, Mode::Finish);
  EXPECT_FALSE(r.planner.current().valid);
  Room far(true);
  PlanHints f;
  f.gaps.push_back(wall_gap(40.0, 40.0));  // no lattice cell sees it from the viewing band
  EXPECT_EQ(far.planner.replan(0, far.user, far.cov, far.nav, far.K, 1.4, f).mode, Mode::Finish);
}

TEST(Steer, HopAndClearanceFiltersApplyOnlyWhenSteering) {
  Room on(true, false), off(false, false);
  auto a = on.planner.replan(0, on.user, on.cov, on.nav, on.K, 1.4);
  auto b = off.planner.replan(0, off.user, off.cov, off.nav, off.K, 1.4);
  ASSERT_EQ(a.mode, Mode::Target);
  ASSERT_EQ(b.mode, Mode::Target);
  // Steering: the hop is either within reach (rotate / tilt in place) or at least min_hop, and keeps clear of unseen cells.
  EXPECT_TRUE(a.path_m <= 0.5 + 1e-9 || a.path_m >= 1.0 - 1e-9) << a.path_m;
  int ix, iz;
  on.nav.to_cell({a.target.x, 0, a.target.z}, ix, iz);
  EXPECT_GE(on.nav.clearance(ix, iz), 0.6);
  EXPECT_FALSE(a.target.gap);
}

TEST(Steer, GapInstructionsNameTheLandmarkAndFallBackWhenLong) {
  auto mk = [](const char* why, double tx, double tz) {
    Situation s;
    s.t_ns = 0;
    s.plan.mode = Mode::Target;
    s.user.pos = {0, 1.4, 0};
    s.user.yaw = 0;
    s.plan.target = {true, false, tx, tz, 0, 0, 10, 1, 10, 0, -1, 1};
    s.plan.target.gap = true, s.plan.target.why = why;
    return s;
  };
  {
    InstructionGenerator g(GuideParams{});
    std::vector<Instruction> out;
    std::vector<LifeEvent> ev;
    g.update(mk("the door wall", 0, 3), out, ev);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].type, InstrType::MoveTo);
    EXPECT_EQ(out[0].msg_id, "gap.move");
    EXPECT_NE(out[0].speak_text.find("the door wall"), std::string::npos) << out[0].speak_text;
    EXPECT_LE(words(out[0].speak_text), 8);
  }
  {
    InstructionGenerator g(GuideParams{});
    std::vector<Instruction> out;
    std::vector<LifeEvent> ev;
    g.update(mk("the door wall", 3, 0), out, ev);  // target far to the left: a turn first
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].type, InstrType::RotateToward);
    EXPECT_EQ(out[0].msg_id, "gap.turn");
    EXPECT_LE(words(out[0].speak_text), 8);
  }
  {
    InstructionGenerator g(GuideParams{});
    std::vector<Instruction> out;
    std::vector<LifeEvent> ev;
    g.update(mk("behind the big old brown wooden sofa by the tall window", 0, 3), out, ev);  // would exceed 8 words
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].msg_id, "move.to");
    EXPECT_EQ(out[0].speak_text.find("sofa"), std::string::npos);
  }
  {
    Situation s = mk("", 0, 3);  // no landmark: plain phrase
    InstructionGenerator g(GuideParams{});
    std::vector<Instruction> out;
    std::vector<LifeEvent> ev;
    g.update(s, out, ev);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].msg_id, "move.to");
  }
  // Every gap phrasing of the catalog is <= 8 words with a worst-case landmark of 3 words (the labeler's longest is shorter).
  for (auto id : {"gap.move", "gap.turn"})
    for (int v = 0; v < InstructionGenerator::variant_count(id); ++v) {
      auto t = InstructionGenerator::render(id, {{"dir", "left"}, {"m", "1.5"}, {"deg", "90"}, {"why", "the door wall"}}, false, uint32_t(v));
      EXPECT_EQ(t.find('{'), std::string::npos) << t;
      auto l = InstructionGenerator::render(id, {{"dir", "left"}, {"m", "1"}, {"deg", "90"}, {"why", "the door wall"}}, true, uint32_t(v));
      EXPECT_EQ(l.find('{'), std::string::npos) << l;
    }
}

TEST(Steer, OverlayKeysOnlyWhenPresentFlatAndUnder4KB) {
  OverlayModel empty;
  const std::string e = empty.to_json();
  for (auto k : {"\"openings\"", "\"gaps\"", "\"gap_why_id\"", "\"gap_why_text\""})  // v3: shell keys always present
    EXPECT_EQ(e.find(k), std::string::npos) << k;
  EXPECT_NE(e.find("\"shell\":\"\""), std::string::npos) << e;
  EXPECT_NE(e.find("\"shell_closed\":false"), std::string::npos) << e;
  EXPECT_NE(e.find("\"spans\":\"\""), std::string::npos) << e;
  // Everything at its cap on top of 12 walls, 24 spans and 12 objects.
  OverlayModel m;
  m.why_id = "why.wall_left", m.why_text = "Left wall not scanned yet";
  for (size_t i = 0; i < OverlayModel::kMaxWalls; ++i) m.wall_list.push_back({-12.34, 56.78, -90.12, 34.56, 100, int(i + 1)});
  for (size_t i = 0; i < OverlayModel::kMaxSpans; ++i) m.span_list.push_back({-int(i + 1), -12.34, 56.78, -90.12, 34.56, 'b'});
  for (size_t i = 0; i < OverlayModel::kMaxObjects; ++i) m.object_list.push_back({int(i + 1), "dining_table", -12.34, 56.78, 9.99, 9.99, 2.5, 89});
  for (size_t i = 0; i < OverlayModel::kMaxOpenings + 4; ++i) m.opening_list.push_back({"window", "W12", -12.34, 56.78, -90.12, 34.56, 0.9, 2.1});
  for (size_t i = 0; i < OverlayModel::kMaxCorners; ++i) m.shell_corners.push_back({-12.34, 56.78});
  m.shell_closed = true;
  for (size_t i = 0; i < OverlayModel::kMaxGaps; ++i) m.gap_list.push_back({"unseen_corner", -12.34, 56.78, 0.9});
  m.gap_why_id = "gap.unseen_corner", m.gap_why_text = "the corner near the window";
  const std::string j = m.to_json();
  EXPECT_LT(j.size(), 4096u) << j.size();
  auto flat = json::parse_flat(j);
  ASSERT_TRUE(flat.has_value()) << j;
  EXPECT_NE(j.find("\"openings\":\"window,W12,-12.34,56.78,-90.12,34.56,0.90,2.10;"), std::string::npos) << j;
  EXPECT_NE(j.find("\"shell\":\"-12.34,56.78;"), std::string::npos);
  EXPECT_NE(j.find("\"shell_closed\":true"), std::string::npos);
  EXPECT_NE(j.find("\"gaps\":\"unseen_corner,-12.34,56.78,0.90;"), std::string::npos);
  EXPECT_NE(j.find("\"gap_why_id\":\"gap.unseen_corner\""), std::string::npos);
  EXPECT_NE(j.find("\"gap_why_text\":\"the corner near the window\""), std::string::npos);
  // Defence in depth: non-finite and delimiter-carrying values are dropped, never emitted.
  OverlayModel bad;
  bad.opening_list.push_back({"door", "W1", std::nan(""), 0, 0, 0, 0, 0});
  bad.opening_list.push_back({"do,or", "W1", 0, 0, 0, 0, 0, 0});
  bad.shell_corners.push_back({std::numeric_limits<double>::infinity(), 0});
  bad.gap_list.push_back({"missing_wall", std::nan(""), 0, 0.5});
  const std::string bj = bad.to_json();
  for (auto k : {"\"openings\"", "\"gaps\""}) EXPECT_EQ(bj.find(k), std::string::npos) << bj;
  EXPECT_NE(bj.find("\"shell\":\"\""), std::string::npos) << bj;  // v3: present, the non-finite corner dropped
  bad.span_list.push_back({1, std::nan(""), 0, 1, 1, 'o'});
  bad.span_list.push_back({1, 0, 0, 1, 1, 'x'});
  EXPECT_NE(bad.to_json().find("\"spans\":\"\""), std::string::npos) << bad.to_json();
}

namespace {
bundle::PoseSample pose_at(int64_t t, const Vec3& p, double yaw, double pitch) {
  auto T = pose_from_yaw_pitch(p, yaw, pitch);
  bundle::PoseSample s;
  s.t_ns = t;
  for (int i = 0; i < 3; ++i) s.t_world_from_camera[i] = T.translation()[i];
  auto q = T.se3().unit_quaternion();
  s.q_world_from_camera_wxyz[0] = q.w(), s.q_world_from_camera_wxyz[1] = q.x();
  s.q_world_from_camera_wxyz[2] = q.y(), s.q_world_from_camera_wxyz[3] = q.z();
  return s;
}

/// A 5 x 4 m box (x 1..6, z -2..2) of which the west wall (x = 1) was never seen, floor and ceiling known.
void feed_box_missing_west(GuideEngine& e) {
  bundle::IntrinsicsSample k;
  k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 319.5, k.cy = 239.5;
  e.on_intrinsics(k);
  e.on_plane(plane("f", "floor", {3.5, 0.0, 0}, {0, 1, 0}, {1, 0, 0}, 5, 4));
  e.on_plane(plane("c", "ceiling", {3.5, 2.5, 0}, {0, -1, 0}, {1, 0, 0}, 5, 4));
  e.on_plane(wall_plane("w0", 1.0, -2.0, 6.0, -2.0));
  e.on_plane(wall_plane("w1", 6.0, -2.0, 6.0, 2.0));
  e.on_plane(wall_plane("w2", 6.0, 2.0, 1.0, 2.0));
  for (int64_t i = 0; i <= 40; ++i) e.on_pose(pose_at(i * 50'000'000, {4.0, 1.4, 0.0}, 0.0, 0.0));
}
}  // namespace

TEST(Steer, EngineOwnsShellAndGapsOnlyAtFlag2) {
  GuideParams p2;
  p2.understanding = 2;
  GuideEngine e(p2);
  feed_box_missing_west(e);
  std::vector<Instruction> out;
  e.update(2 * S, out);
  const ShellModel& sh = e.shell();
  EXPECT_EQ(sh.walls.size(), 4u);
  EXPECT_FALSE(e.gaps().empty());
  bool missing = false;
  for (auto& g : e.gaps()) missing |= g.type == GapType::MissingWall && std::fabs(0.5 * (g.x0 + g.x1) - 1.0) < 0.3;
  EXPECT_TRUE(missing) << "the unseen west wall is a gap";
  const std::string oj = e.overlay(2 * S).to_json();
  EXPECT_NE(oj.find("\"shell\":\""), std::string::npos) << oj;
  EXPECT_NE(oj.find("\"gaps\":\"missing_wall,"), std::string::npos) << oj;
  EXPECT_NE(oj.find("\"gap_why_id\":\"gap.missing_wall\""), std::string::npos) << oj;
  EXPECT_LT(oj.size(), 4096u);
  ASSERT_TRUE(json::parse_flat(oj).has_value()) << oj;
  EXPECT_NE(e.report_json().find("\"steer\":{"), std::string::npos);
  // Deterministic.
  GuideEngine f(p2);
  feed_box_missing_west(f);
  std::vector<Instruction> out2;
  f.update(2 * S, out2);
  EXPECT_EQ(f.overlay(2 * S).to_json(), oj);
  EXPECT_EQ(f.report_json(), e.report_json());
  // Flags 0 and 1 never build gaps or their keys; v3 (ADR 0026): the layout shell is built at every level.
  for (double fl : {0.0, 1.0}) {
    GuideParams p;
    p.understanding = fl;
    GuideEngine g(p);
    feed_box_missing_west(g);
    std::vector<Instruction> o;
    g.update(2 * S, o);
    EXPECT_TRUE(g.gaps().empty()) << fl;
    EXPECT_FALSE(g.shell().walls.empty()) << fl;
    const std::string j = g.overlay(2 * S).to_json();
    for (auto k : {"\"openings\"", "\"gaps\"", "\"gap_why_id\"", "\"gap_why_text\""}) EXPECT_EQ(j.find(k), std::string::npos) << fl << " " << k;
    EXPECT_NE(j.find("\"shell\":\""), std::string::npos) << fl;
    EXPECT_EQ(g.report_json().find("\"steer\""), std::string::npos) << fl;
  }
}

TEST(Steer, EngineRefreshesShellAtMostOncePerEventSecond) {
  GuideParams p2;
  p2.understanding = 2;
  GuideEngine e(p2);
  feed_box_missing_west(e);
  std::vector<Instruction> out;
  e.update(1 * S, out);
  const size_t n = e.gaps().size();
  ASSERT_GT(n, 0u);
  // A wall observed within the same second does not change the gaps until the next refresh.
  e.on_plane(wall_plane("w3", 1.0, 2.0, 1.0, -2.0));
  e.update(1 * S + 300'000'000, out);
  EXPECT_EQ(e.gaps().size(), n);
  e.update(2 * S + 100'000'000, out);
  bool missing = false;
  for (auto& g : e.gaps()) missing |= g.type == GapType::MissingWall;
  EXPECT_FALSE(missing) << "the west wall is now observed";
}

// ---- simulator: flag 2 is deterministic, safe and keeps the overlay small ----
namespace {
double num_after(const std::string& r, const std::string& key, size_t from = 0) {
  auto a = r.find("\"" + key + "\":", from);
  return a == std::string::npos ? -1e9 : std::atof(r.c_str() + a + key.size() + 3);
}

sim::Config sim_cfg(const std::string& room, uint64_t seed, double max_s, double flag) {
  sim::Config c;
  c.room = room, c.seed = seed, c.max_s = max_s;
  c.guide.understanding = flag;
  c.human.hold_level = true;
  c.view_points = true;
  c.detector_oracle = true;
  return c;
}

/// coverage_series_10s: [[t, c], ...] every 10 s of sim time.
std::vector<std::pair<double, double>> series_of(const std::string& r) {
  std::vector<std::pair<double, double>> s;
  auto a = r.find("\"coverage_series_10s\":[");
  if (a == std::string::npos) return s;
  size_t i = r.find('[', a + 22);
  const size_t end = r.find("]]", i);
  while (i != std::string::npos && i < end + 1) {
    i = r.find('[', i + 1);
    if (i == std::string::npos || i > end) break;
    s.push_back({std::atof(r.c_str() + i + 1), std::atof(r.c_str() + r.find(',', i) + 1)});
  }
  return s;
}

double cov_at(const std::vector<std::pair<double, double>>& s, double t) {
  double c = 0;
  for (auto& [ts, cs] : s)
    if (ts <= t + 1e-9) c = cs;
  return c;
}
}  // namespace

TEST(Steer, Flag2SimIsDeterministicSafeAndSmall) {
  std::string a, b, off;
  std::thread t1([&] { a = sim::simulate(sim_cfg("box", 1, 90, 2)); });
  std::thread t2([&] { b = sim::simulate(sim_cfg("box", 1, 90, 2)); });
  std::thread t3([&] { off = sim::simulate(sim_cfg("box", 1, 90, 0)); });
  t1.join(), t2.join(), t3.join();
  EXPECT_EQ(a, b) << "replay twice: byte-identical";
  EXPECT_EQ(num_after(a, "unsafe_targets_gt"), 0);
  EXPECT_EQ(num_after(off, "unsafe_targets_gt"), 0);
  EXPECT_NE(a.find("\"steer\":{"), std::string::npos);
  EXPECT_EQ(off.find("\"steer\":{"), std::string::npos) << "flag 0 report is the Phase 8 one";
}

// The acceptance comparison (box / L / furniture x seeds 1..3, flag 0 / 1 / 2). Long (minutes), so it only runs with
// CKIT_STEER_EVAL=1; the table goes into ADR 0027 (real-room-fix T2). "Not worse" = flag 2 is within 0.02 of flag 0 on mean coverage at
// every checkpoint and does not take longer to 90 % (censored at the horizon).
TEST(Steer, GuideEvalFlag012Comparison) {
  if (!std::getenv("CKIT_STEER_EVAL")) GTEST_SKIP() << "set CKIT_STEER_EVAL=1 to run the flag comparison";
  const double horizon = std::getenv("CKIT_STEER_EVAL_S") ? std::atof(std::getenv("CKIT_STEER_EVAL_S")) : 240.0;
  struct Job {
    std::string room;
    uint64_t seed;
    double flag;
  };
  std::vector<Job> jobs;
  for (auto room : {"box", "L", "furniture"})
    for (uint64_t seed : {1, 2, 3})
      for (double flag : {0.0, 1.0, 2.0}) jobs.push_back({room, seed, flag});
  std::vector<std::string> rep(jobs.size());
  const size_t nth = std::max<size_t>(1, std::min<size_t>(8, std::thread::hardware_concurrency()));
  std::atomic<size_t> next{0};
  std::vector<std::thread> th;
  for (size_t w = 0; w < nth; ++w)
    th.emplace_back([&] {
      for (size_t k; (k = next++) < jobs.size();) rep[k] = sim::simulate(sim_cfg(jobs[k].room, jobs[k].seed, horizon, jobs[k].flag));
    });
  for (auto& t : th) t.join();
  const std::vector<double> marks = {30, 60, 120, 180, 240};
  std::map<double, std::vector<double>> sums, t90;
  std::map<double, int> n;
  std::map<double, double> unsafe, gap_targets;
  std::cout << "\n  Steer.GuideEvalFlag012Comparison (hold_level, view_points, detector oracle, horizon " << horizon << " s)\n"
            << "  | room | seed | flag | cov@30 | cov@60 | cov@120 | cov@180 | cov@240 | t90 s | final | unsafe | gap targets |\n"
            << "  |---|---|---|---|---|---|---|---|---|---|---|---|\n";
  for (size_t k = 0; k < jobs.size(); ++k) {
    auto s = series_of(rep[k]);
    const double t90k = num_after(rep[k], "t90_s");
    const double censored = t90k < 0 ? horizon : t90k;
    std::cout << "  | " << jobs[k].room << " | " << jobs[k].seed << " | " << jobs[k].flag;
    auto& v = sums[jobs[k].flag];
    v.resize(marks.size(), 0.0);
    for (size_t m = 0; m < marks.size(); ++m) {
      const double c = cov_at(s, marks[m]);
      v[m] += c;
      std::cout << " | " << c;
    }
    ++n[jobs[k].flag];
    t90[jobs[k].flag].push_back(censored);
    const double un = num_after(rep[k], "unsafe_targets_gt");
    unsafe[jobs[k].flag] += un;
    const auto st = rep[k].find("\"steer\":{");
    const double gt = st == std::string::npos ? 0 : num_after(rep[k], "gap_targets", st);
    gap_targets[jobs[k].flag] += gt;
    std::cout << " | " << t90k << " | " << num_after(rep[k], "coverage_gt_final") << " | " << un << " | " << gt << " |\n";
  }
  std::cout << "  | flag | mean cov@30 | @60 | @120 | @180 | @240 | mean t90 (censored) | unsafe | gap targets |\n  |---|---|---|---|---|---|---|---|---|\n";
  std::map<double, double> mt90;
  for (auto& [flag, v] : sums) {
    double s90 = 0;
    for (double x : t90[flag]) s90 += x;
    mt90[flag] = s90 / n[flag];
    std::cout << "  | " << flag;
    for (double x : v) std::cout << " | " << x / n[flag];
    std::cout << " | " << mt90[flag] << " | " << unsafe[flag] << " | " << gap_targets[flag] << " |\n";
  }
  for (size_t m = 0; m < marks.size(); ++m)
    EXPECT_GE(sums[2.0][m] / n[2.0], sums[0.0][m] / n[0.0] - 0.02) << "coverage at " << marks[m] << " s: flag 2 is worse than flag 0";
  EXPECT_LE(mt90[2.0], mt90[0.0] + 5.0) << "time to 90 %";
  EXPECT_EQ(unsafe[2.0], 0);
}
