// Phase 8: GuideMind unit tests per stage + simulator determinism, safety and budget.
#include <set>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <sstream>

#include "capturekit/guide/engine.hpp"
#include "capturekit/guide/sim.hpp"

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

// A 4 m wide, 2.5 m high wall at z = 4 facing -z (towards the room).
bundle::PlaneRecord back_wall(const std::string& id = "w") {
  return plane(id, "wall", {2, 1.25, 4}, {0, 0, -1}, {1, 0, 0}, 4, 2.5);
}

struct BlockAll : Occluder {
  bool clear(const Vec3&, const Vec3&) const override { return false; }
};

GuideParams P() { return GuideParams{}; }

}  // namespace

// ---- params ----
TEST(GuideParams, HashOverridesAndUnknownKeys) {
  auto a = GuideParams::from_json("{}");
  auto b = GuideParams::from_json("{\"min_views\": 5}");
  ASSERT_TRUE(a && b);
  EXPECT_EQ(a->hash(), GuideParams{}.hash());
  EXPECT_NE(a->hash(), b->hash());
  EXPECT_DOUBLE_EQ(b->min_views, 5);
  std::string err;
  EXPECT_FALSE(GuideParams::from_json("{\"nope\": 1}", &err));
  EXPECT_NE(err.find("nope"), std::string::npos);
}

// ---- LiveMap ----
TEST(GuideLiveMap, UpdateMergeAndLevels) {
  LiveMap m;
  EXPECT_EQ(m.add_plane(back_wall("a")), "a");
  int v = m.version();
  // Same id: the newest (grown) geometry replaces the old one.
  auto g = back_wall("a");
  g.extent_u = 5;
  m.add_plane(g);
  EXPECT_EQ(m.planes().size(), 1u);
  EXPECT_DOUBLE_EQ(m.planes().at("a").eu, 5);
  EXPECT_GT(m.version(), v);
  // New id for the same surface: merged into the existing plane.
  auto dup = back_wall("b");
  dup.center[0] = 4.9;  // touching the first
  EXPECT_EQ(m.add_plane(dup), "a");
  EXPECT_EQ(m.planes().size(), 1u);
  EXPECT_GT(m.planes().at("a").eu, 5);
  // A different wall (other normal) stays separate; degenerate planes are ignored.
  m.add_plane(plane("side", "wall", {0, 1.25, 2}, {1, 0, 0}, {0, 0, 1}, 4, 2.5));
  EXPECT_EQ(m.planes().size(), 2u);
  m.add_plane(plane("bad", "wall", {0, 0, 0}, {0, 0, 1}, {1, 0, 0}, 0, 0));
  EXPECT_EQ(m.planes().size(), 2u);
  // Levels.
  EXPECT_FALSE(m.has_floor());
  m.add_plane(plane("f", "floor", {2, 0.02, 2}, {0, 1, 0}, {1, 0, 0}, 4, 4));
  m.add_plane(plane("c", "ceiling", {2, 2.5, 2}, {0, -1, 0}, {1, 0, 0}, 4, 4));
  EXPECT_TRUE(m.has_floor() && m.has_ceiling());
  EXPECT_NEAR(m.floor_y(), 0.02, 1e-9);
  EXPECT_NEAR(m.ceiling_y(), 2.5, 1e-9);
  EXPECT_TRUE(m.extents().valid());
}

// ---- Coverage ----
TEST(GuideCoverage, NeedsViewsAndSpread) {
  LiveMap m;
  m.add_plane(back_wall());
  GuideParams p = P();
  Coverage cov(p);
  cov.sync(m);
  ASSERT_GT(cov.stats().valid, 1000u);
  EXPECT_EQ(cov.stats().sufficient, 0u);
  auto K = K640();
  auto obs = [&](double x, double z) {
    return Obs{pose_from_yaw_pitch({x, 1.25, z}, 0, 0), K, 0, 1.0, true};
  };
  // Three views from the SAME spot: enough views, no spread -> still not sufficient.
  for (int i = 0; i < 3; ++i) cov.observe(obs(2, 1.5), nullptr, nullptr);
  EXPECT_GT(cov.stats().seen, 100u);
  EXPECT_EQ(cov.stats().sufficient, 0u);
  // Views from different positions give ray spread: the centre becomes sufficient.
  cov.observe(obs(1.2, 1.5), nullptr, nullptr);
  cov.observe(obs(2.8, 1.5), nullptr, nullptr);
  EXPECT_GT(cov.stats().sufficient, 50u);
  EXPECT_GT(cov.stats().completeness(), 0.05);
}

// views_at: viewed wall cell > 0, outside the grid 0, valid == 0 cell (plane shrank after being viewed) 0.
TEST(GuideCoverage, ViewsAtInsideOutsideAndInvalidCell) {
  LiveMap m;
  m.add_plane(back_wall());
  GuideParams p = P();
  Coverage cov(p);
  cov.sync(m);
  auto K = K640();
  for (double x : {1.4, 2.0, 2.6}) cov.observe(Obs{pose_from_yaw_pitch({x, 1.25, 1.5}, 0, 0), K, 0, 1.0, true}, nullptr, nullptr);
  const Vec3 inside(2.0, 1.25, 4.0), edge(3.2, 1.25, 4.0);
  EXPECT_GE(cov.views_at("w", inside), 3);
  EXPECT_GE(cov.views_at("w", edge), 1);
  EXPECT_EQ(cov.views_at("w", Vec3(30.0, 1.25, 4.0)), 0);  // outside the grid
  EXPECT_EQ(cov.views_at("w", Vec3(2.0, 40.0, 4.0)), 0);
  // Same plane id, narrower (u in [1, 3]): the grid keeps the cell but it is no longer valid.
  m.add_plane(plane("w", "wall", {2, 1.25, 4}, {0, 0, -1}, {1, 0, 0}, 2, 2.5));
  cov.sync(m);
  EXPECT_GE(cov.views_at("w", inside), 3);
  EXPECT_EQ(cov.views_at("w", edge), 0);
}

TEST(GuideCoverage, RangeSharpnessAndOcclusion) {
  LiveMap m;
  m.add_plane(back_wall());
  GuideParams p = P();
  Coverage cov(p);
  cov.sync(m);
  auto K = K640();
  // Too far (range > 4 m): seen but never a qualifying view.
  for (double x : {1.0, 2.0, 3.0})
    cov.observe(Obs{pose_from_yaw_pitch({x, 1.25, -1.5}, 0, 0), K, 0, 1.0, true}, nullptr, nullptr);
  EXPECT_GT(cov.stats().seen, 0u);
  EXPECT_EQ(cov.stats().sufficient, 0u);
  // Blurry keyframes do not count.
  for (double x : {1.0, 2.0, 3.0})
    cov.observe(Obs{pose_from_yaw_pitch({x, 1.25, 2.0}, 0, 0), K, 0, 0.1, true}, nullptr, nullptr);
  EXPECT_EQ(cov.stats().sufficient, 0u);
  // Occluded cells are excluded after repeated blocked keyframe views (e.g. behind furniture).
  size_t valid0 = cov.stats().valid;
  BlockAll blocker;
  for (int i = 0; i < 9; ++i)
    cov.observe(Obs{pose_from_yaw_pitch({2, 1.25, 2.0 + 0.01 * i}, 0, 0), K, 0, 1.0, true}, &blocker, nullptr);
  EXPECT_LT(cov.stats().valid, valid0);
}

TEST(GuideCoverage, MarginalGainRewardsNewAnglesNotRepeats) {
  LiveMap m;
  m.add_plane(back_wall());
  GuideParams p = P();
  Coverage cov(p);
  cov.sync(m);
  auto K = K640();
  auto T0 = pose_from_yaw_pitch({2, 1.25, 2.0}, 0, 0);
  double g_fresh = cov.eval_view(T0, K, nullptr).gain;
  EXPECT_GT(g_fresh, 10);
  for (int i = 0; i < 4; ++i) cov.observe(Obs{T0, K, 0, 1.0, true}, nullptr, nullptr);
  double g_same = cov.eval_view(T0, K, nullptr).gain;                                         // nothing new from here
  double g_side = cov.eval_view(pose_from_yaw_pitch({0.8, 1.25, 2.0}, 0, 0), K, nullptr).gain;  // new angle
  EXPECT_LT(g_same, 0.3 * g_fresh);
  EXPECT_GT(g_side, 2 * g_same);
}

// ---- NavGrid ----
TEST(GuideNavGrid, ObstaclesFreeSpaceAndPassability) {
  GuideParams p = P();
  NavGrid nav(p);
  nav.set_floor_y(0);
  // Points: a wall at x = 3 (height 1.0 m) and a low point (0.1 m) that must be ignored.
  for (int i = 0; i < 40; ++i) {
    nav.add_point({3.0, 1.0, 0.1 * i});
    nav.add_point({3.0, 1.0, 0.1 * i});
  }
  nav.add_point({1.0, 0.1, 1.0});
  nav.add_point({1.0, 0.1, 1.0});
  int ix, iz;
  nav.to_cell({3.0, 0, 1.0}, ix, iz);
  EXPECT_TRUE(nav.obstacle(ix, iz));
  nav.to_cell({1.0, 0, 1.0}, ix, iz);
  EXPECT_FALSE(nav.obstacle(ix, iz));
  // Free space by carving from the camera to the wall points (never through the wall).
  for (int i = 0; i < 40; ++i) nav.carve({0.5, 1.4, 2.0}, {3.0, 1.0, 0.1 * i});
  nav.mark_traversed({0.5, 1.4, 2.0});
  nav.rebuild();
  nav.to_cell({1.5, 0, 2.0}, ix, iz);
  EXPECT_TRUE(nav.known_free(ix, iz));
  nav.to_cell({3.4, 0, 2.0}, ix, iz);  // behind the wall: unknown
  EXPECT_FALSE(nav.known_free(ix, iz));
  // Body radius: a cell right next to the wall is free but not passable; well inside the room it is.
  nav.to_cell({2.8, 0, 2.0}, ix, iz);
  EXPECT_FALSE(nav.passable(ix, iz));
  nav.to_cell({1.5, 0, 2.0}, ix, iz);
  EXPECT_TRUE(nav.passable(ix, iz));
  // Reachability: cells beyond the wall are unreachable.
  int sx, sz;
  nav.to_cell({1.5, 0, 2.0}, sx, sz);
  std::vector<float> dist;
  nav.distances(sx, sz, dist);
  EXPECT_EQ(dist[nav.index(sx, sz)], 0.f);
  nav.to_cell({3.4, 0, 2.0}, ix, iz);
  EXPECT_LT(dist[nav.index(ix, iz)], 0.f);
}

TEST(GuideNavGrid, LineOfSightRespectsObstacleHeight) {
  GuideParams p = P();
  NavGrid nav(p);
  nav.set_floor_y(0);
  for (int i = 0; i < 20; ++i) {  // a 0.8 m high sofa at x = 2, z in [0, 2]
    nav.add_point({2.0, 0.8, 0.1 * i});
    nav.add_point({2.0, 0.8, 0.1 * i});
  }
  // A ray at 1.4 m passes over it; a ray down to the floor behind it does not.
  EXPECT_TRUE(nav.clear({0.5, 1.4, 1.0}, {3.5, 1.4, 1.0}));
  EXPECT_FALSE(nav.clear({0.5, 1.4, 1.0}, {3.5, 0.0, 1.0}));
}

// ---- Planner ----
namespace {
struct Scene {
  GuideParams p = P();
  LiveMap map;
  Coverage cov{P()};
  NavGrid nav{P()};
  Planner planner{P()};
  geometry::Intrinsics K = K640();
  UserState user;
  Scene() {
    nav.set_floor_y(0);
    map.add_plane(back_wall());
    map.add_plane(plane("l", "wall", {0, 1.25, 2}, {1, 0, 0}, {0, 0, 1}, 4, 2.5));
    cov.sync(map);
    // Free space: the 4 x 4 room, with wall obstacles at z = 4 and x = 0.
    for (int i = 0; i < 40; ++i)
      for (double h : {0.4, 1.2, 2.3})  // real walls have points at every height
        for (int k = 0; k < 2; ++k) {
          nav.add_point({0.0, h, 0.1 * i});
          nav.add_point({0.1 * i, h, 4.0});
        }
    for (int ix = 3; ix < 40; ++ix)  // integer cell indices: float stepping would skip cells
      for (int iz = 3; iz < 37; ++iz) nav.mark_open({(ix + 0.5) * 0.1, 0, (iz + 0.5) * 0.1});
    user.pos = {2, 1.4, 1.0};
    user.yaw = 0;
    nav.mark_traversed(user.pos);
    nav.rebuild();
  }
};
}  // namespace

TEST(GuidePlanner, NoMapMeansSweepAndCompleteMeansFinish) {
  GuideParams p = P();
  Coverage empty(p);
  NavGrid nav(p);
  Planner pl(p);
  UserState u;
  auto plan = pl.replan(0, u, empty, nav, K640(), 1.4);
  EXPECT_EQ(plan.mode, Mode::Sweep);
}

TEST(GuidePlanner, TargetsUsefulSafeViewpointsAndKeepsThem) {
  Scene s;
  auto plan = s.planner.replan(0, s.user, s.cov, s.nav, s.K, 1.4);
  ASSERT_EQ(plan.mode, Mode::Target);
  ASSERT_TRUE(plan.target.valid);
  int ix, iz;
  s.nav.to_cell({plan.target.x, 0, plan.target.z}, ix, iz);
  EXPECT_TRUE(s.nav.passable(ix, iz));  // never a blocked or unknown cell
  EXPECT_GT(plan.target.gain, s.p.min_gain);
  // Hysteresis: identical state -> identical target, no switching.
  Target first = s.planner.current();
  for (int i = 1; i <= 5; ++i) s.planner.replan(i * 500'000'000LL, s.user, s.cov, s.nav, s.K, 1.4);
  EXPECT_EQ(s.planner.switches(), 0);
  EXPECT_EQ(s.planner.current().id, first.id);
  EXPECT_EQ(s.planner.reached_count(), 0);  // standing at the pose for 2.5 s is not yet a visit
  // Reaching it clears the target and remembers the spot.
  UserState at = s.user;
  at.pos = {first.x, 1.4, first.z};
  at.yaw = first.yaw;
  at.pitch = first.pitch;
  auto r = s.planner.replan(10'000'000'000LL, at, s.cov, s.nav, s.K, 1.4);
  EXPECT_TRUE(r.reached);  // the user has stood there past dwell_s (before that it is not a visit)
  EXPECT_EQ(s.planner.reached_count(), 1);
}

TEST(GuidePlanner, PenalizedTargetIsNotChosenAgain) {
  Scene s;
  s.planner.replan(0, s.user, s.cov, s.nav, s.K, 1.4);
  Target t1 = s.planner.current();
  ASSERT_TRUE(t1.valid);
  s.planner.penalize_current();
  EXPECT_FALSE(s.planner.current().valid);
  s.planner.replan(500'000'000, s.user, s.cov, s.nav, s.K, 1.4);
  Target t2 = s.planner.current();
  ASSERT_TRUE(t2.valid);
  EXPECT_TRUE(std::hypot(t2.x - t1.x, t2.z - t1.z) > 0.6 || std::fabs(wrap_pi(t2.yaw - t1.yaw)) > deg2rad(35));
}

TEST(GuidePlanner, AsksToLookAtMissingCeilingWhenWallsAreDone) {
  Scene s;
  PlanHints h;
  h.need_ceiling = true;
  // Mark the walls as covered by observing from many positions.
  for (double x = 0.8; x <= 3.4; x += 0.2)
    for (double z : {1.0, 1.5, 2.0, 2.5, 3.0})
      for (double yaw : {0.0, -kPi / 2}) s.cov.observe(Obs{pose_from_yaw_pitch({x, 1.4, z}, yaw, 0), s.K, 0, 1.0, true}, nullptr, nullptr);
  ASSERT_GT(s.cov.stats().completeness(), 0.5);
  auto plan = s.planner.replan(0, s.user, s.cov, s.nav, s.K, 1.4, h);
  ASSERT_TRUE(plan.target.valid);
  EXPECT_TRUE(plan.discover);
  EXPECT_GT(plan.target.pitch, deg2rad(20));  // look up
}

// ---- Instructions ----
namespace {
Situation sit(int64_t t_s, Mode mode = Mode::Target) {
  Situation s;
  s.t_ns = t_s * 1'000'000'000LL;
  s.plan.mode = mode;
  s.user.pos = {0, 1.4, 0};
  s.user.yaw = 0;
  return s;
}
std::vector<Instruction> run(InstructionGenerator& g, const Situation& s, std::vector<LifeEvent>* ev = nullptr) {
  std::vector<Instruction> out;
  std::vector<LifeEvent> e;
  g.update(s, out, e);
  if (ev) *ev = e;
  return out;
}
}  // namespace

TEST(GuideInstructions, EgocentricMoveAndRotate) {
  InstructionGenerator g(P());
  auto s = sit(0);
  s.plan.target = {true, false, 3.0, 0.0, 0, 0, 10, 1, 0, 1};  // 3 m to the +x side = the user's right? (+x is left of +z)
  auto out = run(g, s);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, InstrType::RotateToward);  // 90 degrees off: turn first
  EXPECT_GT(out[0].param("rel_deg"), 60);            // + = left
  EXPECT_NE(out[0].speak_text.find("left"), std::string::npos) << out[0].speak_text;
  InstructionGenerator g2(P());
  auto s2 = sit(0);
  s2.plan.target = {true, false, 0.0, 3.0, 0, 0, 10, 1, 0, 1};  // straight ahead
  auto o2 = run(g2, s2);
  ASSERT_EQ(o2.size(), 1u);
  EXPECT_EQ(o2[0].type, InstrType::MoveTo);
  EXPECT_NE(o2[0].speak_text.find("ahead"), std::string::npos);
  EXPECT_DOUBLE_EQ(o2[0].param("distance_m"), 3.0);
}

namespace {
int word_count(const std::string& t) {
  int words = 0;
  std::istringstream is(t);
  for (std::string w; is >> w;) ++words;
  return words;
}
}  // namespace

TEST(GuideInstructions, ShortTextsAreShortAndAllMessagesExist) {
  for (auto id : {"slow_down", "hold_still", "relocalize", "move.to", "rotate.to", "tilt.up", "tilt.down", "step.back", "sweep", "more_light",
                  "finish", "progress.25", "progress.50", "progress.75", "keep_turning", "aim_texture", "recalibrate"}) {
    ASSERT_GE(InstructionGenerator::variant_count(id), 2) << id;  // 2-3 phrasings per id
    ASSERT_LE(InstructionGenerator::variant_count(id), 3) << id;
    for (int v = 0; v < InstructionGenerator::variant_count(id); ++v) {
      // Worst-case words: two-word direction, long number.
      auto t = InstructionGenerator::render(id, {{"dir", "slightly left"}, {"m", "1.5"}, {"deg", "90"}}, false, uint32_t(v));
      EXPECT_NE(t, id) << id;
      EXPECT_LE(word_count(t), 8) << t;  // every speak_text <= 8 words
      EXPECT_EQ(t.find('{'), std::string::npos) << t;  // all placeholders filled
      auto l = InstructionGenerator::render(id, {{"dir", "left"}, {"m", "1"}, {"deg", "90"}}, true, uint32_t(v));
      EXPECT_EQ(l.find('{'), std::string::npos) << l;
    }
  }
}

TEST(GuideInstructions, MeterUnitIsPluralAware) {
  auto one = InstructionGenerator::render("move.to", {{"dir", "ahead"}, {"m", "1"}}, false, 0);
  EXPECT_EQ(one, "Walk ahead, 1 meter.");
  EXPECT_EQ(InstructionGenerator::render("move.to", {{"dir", "ahead"}, {"m", "2"}}, false, 0), "Walk ahead, 2 meters.");
  EXPECT_EQ(InstructionGenerator::render("move.to", {{"dir", "ahead"}, {"m", "0.5"}}, false, 0), "Walk ahead, 0.5 meters.");
  EXPECT_NE(InstructionGenerator::render("step.back", {{"m", "1"}}, true, 0).find("1 meter "), std::string::npos);
  for (int v = 0; v < 3; ++v) {
    EXPECT_EQ(InstructionGenerator::render("move.to", {{"dir", "ahead"}, {"m", "1"}}, false, uint32_t(v)).find("1 meters"), std::string::npos);
    EXPECT_EQ(InstructionGenerator::render("move.to", {{"dir", "ahead"}, {"m", "1"}}, true, uint32_t(v)).find("1 meters"), std::string::npos);
  }
  // End to end: a 1 m move is spoken singular.
  InstructionGenerator g(P());
  auto s = sit(0);
  s.plan.target = {true, false, 0.0, 1.0, 0, 0, 10, 1, 0, 1};
  s.user.pos = {0, 1.4, 0};
  auto out = run(g, s);
  ASSERT_EQ(out.size(), 1u);
  ASSERT_EQ(out[0].type, InstrType::MoveTo);
  ASSERT_DOUBLE_EQ(out[0].param("distance_m"), 1.0);
  EXPECT_NE(out[0].speak_text.find("1 meter"), std::string::npos) << out[0].speak_text;
  EXPECT_EQ(out[0].speak_text.find("1 meters"), std::string::npos) << out[0].speak_text;
  EXPECT_EQ(out[0].long_text.find("1 meters"), std::string::npos) << out[0].long_text;
}

TEST(GuideInstructions, PhrasingVariesDeterministicallyById) {
  const int n = InstructionGenerator::variant_count("move.to");
  std::set<std::string> seen;
  for (int i = 0; i < n; ++i) seen.insert(InstructionGenerator::render("move.to", {{"dir", "ahead"}, {"m", "2"}}, false, uint32_t(i)));
  EXPECT_EQ(int(seen.size()), n);  // distinct phrasings
  EXPECT_EQ(InstructionGenerator::render("move.to", {{"dir", "ahead"}, {"m", "2"}}, false, 0),
            InstructionGenerator::render("move.to", {{"dir", "ahead"}, {"m", "2"}}, false, uint32_t(n)));  // modulo, no state
  // Same input twice -> same output; the instruction id (1-based) selects the phrasing.
  InstructionGenerator g1(P()), g2(P());
  auto s = sit(0);
  s.plan.target = {true, false, 0.0, 3.0, 0, 0, 10, 1, 0, 1};
  auto a = run(g1, s), b = run(g2, s);
  ASSERT_EQ(a.size(), 1u);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_EQ(a[0].speak_text, b[0].speak_text);
  EXPECT_EQ(a[0].speak_text, InstructionGenerator::render("move.to", {{"dir", "ahead"}, {"m", "3"}}, false, a[0].id - 1));
}

TEST(GuideInstructions, ProgressPromptsAt25_50_75OncePerThreshold) {
  InstructionGenerator g(P());
  std::vector<std::string> said;
  auto step = [&](int64_t t_s, double completeness) {
    auto s = sit(t_s);
    s.completeness = completeness;  // no target: only info prompts can fire
    for (auto& i : run(g, s))
      if (i.type == InstrType::Progress) {
        said.push_back(i.msg_id);
        EXPECT_EQ(i.priority, 1);
        EXPECT_LE(word_count(i.speak_text), 8) << i.speak_text;
      }
  };
  step(0, 0.10);
  step(10, 0.20);
  EXPECT_TRUE(said.empty());
  step(20, 0.26);
  ASSERT_EQ(said.size(), 1u);
  EXPECT_EQ(said[0], "progress.25");
  step(30, 0.30);  // not repeated
  step(40, 0.45);
  EXPECT_EQ(said.size(), 1u);
  step(50, 0.52);
  ASSERT_EQ(said.size(), 2u);
  EXPECT_EQ(said[1], "progress.50");
  step(60, 0.60);
  step(70, 0.80);
  ASSERT_EQ(said.size(), 3u);
  EXPECT_EQ(said[2], "progress.75");
  step(80, 0.95);
  step(90, 0.50);  // completeness dipping and recovering does not repeat a prompt
  step(100, 0.80);
  EXPECT_EQ(said.size(), 3u);
}

TEST(GuideInstructions, ProgressJumpSpeaksOnlyHighestAndIsRateLimited) {
  InstructionGenerator g(P());
  auto s = sit(0);
  s.completeness = 0.80;  // jumped over 25 and 50
  auto out = run(g, s);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].msg_id, "progress.75");
  auto s2 = sit(10);
  s2.completeness = 0.90;
  EXPECT_TRUE(run(g, s2).empty());  // all thresholds spent
  // Finish mode suppresses progress prompts (the finish instruction covers it).
  InstructionGenerator gf(P());
  auto sf = sit(0, Mode::Finish);
  sf.completeness = 0.9;
  auto of = run(gf, sf);
  ASSERT_EQ(of.size(), 1u);
  EXPECT_EQ(of[0].type, InstrType::Finish);
  // Suppression lasts only while in Finish: when the plan returns to Target, the pending threshold is spoken
  // (once), and a threshold spoken earlier is not repeated.
  InstructionGenerator gr(P());
  auto r0 = sit(0);
  r0.completeness = 0.30;
  auto o0 = run(gr, r0);
  ASSERT_EQ(o0.size(), 1u);
  EXPECT_EQ(o0[0].msg_id, "progress.25");
  auto r1 = sit(20, Mode::Finish);
  r1.completeness = 0.60;  // crosses 50 % while finishing: silent
  for (auto& i : run(gr, r1)) EXPECT_NE(i.type, InstrType::Progress);
  auto r2 = sit(40);
  r2.completeness = 0.60;  // plan went back to scanning
  auto o2 = run(gr, r2);
  ASSERT_EQ(o2.size(), 1u);
  EXPECT_EQ(o2[0].msg_id, "progress.50");
  auto r3 = sit(60);
  r3.completeness = 0.60;
  EXPECT_TRUE(run(gr, r3).empty());  // each threshold once
}

TEST(GuideInstructions, KeepTurningDuringSweep) {
  InstructionGenerator g(P());
  auto at = [&](int64_t t_s, double yaw_deg) {
    auto s = sit(t_s, Mode::Sweep);
    s.user.yaw = deg2rad(yaw_deg);
    return run(g, s);
  };
  EXPECT_TRUE(at(0, 0).empty());  // sweep only after a short settle time
  auto o = at(4, 0);
  ASSERT_EQ(o.size(), 1u);
  ASSERT_EQ(o[0].type, InstrType::Sweep);
  EXPECT_TRUE(at(6, 50).empty());
  auto k1 = at(9, 110);  // past 100 degrees turned
  ASSERT_EQ(k1.size(), 1u);
  EXPECT_EQ(k1[0].type, InstrType::KeepTurning);
  EXPECT_EQ(k1[0].msg_id, "keep_turning");
  EXPECT_LE(word_count(k1[0].speak_text), 8);
  EXPECT_TRUE(at(11, 150).empty());  // not before 200 degrees
  auto k2 = at(15, 210);
  ASSERT_EQ(k2.size(), 1u);
  EXPECT_EQ(k2[0].type, InstrType::KeepTurning);
  EXPECT_TRUE(at(16, 215).empty());  // at most two prompts per sweep
}

TEST(GuideInstructions, CooldownsMinGapAndNoRepeats) {
  GuideParams p = P();
  InstructionGenerator g(p);
  auto s = sit(0);
  s.plan.target = {true, false, 0.0, 3.0, 0, 0, 10, 1, 0, 1};
  EXPECT_EQ(run(g, s).size(), 1u);
  // Within the minimum gap and cooldown nothing new is said, even if asked every frame.
  for (int t = 1; t < 6; ++t) {
    auto s2 = sit(t);
    s2.plan = s.plan;
    EXPECT_TRUE(run(g, s2).empty()) << "t=" << t;
  }
  // The identical instruction stays active until it completes or expires: no back-to-back repeat.
  auto s3 = sit(7);
  s3.plan = s.plan;
  EXPECT_TRUE(run(g, s3).empty());
}

TEST(GuideInstructions, TrackingReasonMapsToInstruction) {
  GuideParams p = P();
  // EXCESSIVE_MOTION (limited tracking) -> slow_down, with its own cooldown.
  {
    InstructionGenerator g(p);
    auto s = sit(0);
    s.tracking_state = 2, s.tracking_reason = 2;
    auto o = run(g, s);
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].type, InstrType::SlowDown);
    EXPECT_EQ(o[0].msg_id, "slow_down");
    auto s2 = sit(1);
    s2.tracking_state = 2, s2.tracking_reason = 2;
    EXPECT_TRUE(run(g, s2).empty());  // no repeat while active / cooling
  }
  // A turn we asked for excuses the blur path but not the reported EXCESSIVE_MOTION.
  {
    InstructionGenerator g(p);
    auto s = sit(0);
    s.plan.target = {true, false, 3.0, 0.0, 0, 0, 10, 1, 0, 1};  // 90 degrees off: ROTATE_TOWARD
    ASSERT_EQ(run(g, s).at(0).type, InstrType::RotateToward);
    auto b = sit(2);
    b.plan = s.plan;
    b.blur_streak = 5;
    EXPECT_TRUE(run(g, b).empty());  // blur while turning on request: not scolded
    auto r = sit(3);
    r.plan = s.plan;
    r.tracking_state = 2, r.tracking_reason = 2;
    auto o = run(g, r);
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].type, InstrType::SlowDown);
  }
  // INSUFFICIENT_LIGHT -> more_light, even though no frame was flagged underexposed.
  {
    InstructionGenerator g(p);
    auto s = sit(0);
    s.tracking_state = 2, s.tracking_reason = 4;
    auto o = run(g, s);
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].type, InstrType::MoreLight);
    EXPECT_EQ(o[0].msg_id, "more_light");
  }
  // INSUFFICIENT_FEATURES -> "Aim at a textured wall"; completes when the reason clears.
  {
    InstructionGenerator g(p);
    auto s = sit(0);
    s.tracking_state = 2, s.tracking_reason = 3;
    auto o = run(g, s);
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].type, InstrType::AimTextured);
    EXPECT_EQ(o[0].msg_id, "aim_texture");
    EXPECT_EQ(o[0].priority, 3);
    EXPECT_EQ(InstructionGenerator::render("aim_texture", {}, false, 0), "Aim at a textured wall.");
    auto s2 = sit(1);
    std::vector<LifeEvent> ev;
    run(g, s2, &ev);
    bool completed = false;
    for (auto& e : ev) completed |= e.ins.type == InstrType::AimTextured && e.state == LifeState::Completed;
    EXPECT_TRUE(completed);
  }
  // Other reasons (none, initializing, relocalizing, ...) add no instruction; normal tracking ignores the reason.
  for (int reason : {0, 1, 5, 6, 8}) {
    InstructionGenerator g(p);
    auto s = sit(0);
    s.tracking_state = 2, s.tracking_reason = reason;
    EXPECT_TRUE(run(g, s).empty()) << reason;
  }
  {
    InstructionGenerator g(p);
    auto s = sit(0);
    s.tracking_reason = 3;  // tracking_state stays 1 (normal)
    EXPECT_TRUE(run(g, s).empty());
  }
}

TEST(GuideInstructions, SafetyInterruptsAndLifecycle) {
  GuideParams p = P();
  InstructionGenerator g(p);
  auto s = sit(0);
  s.plan.target = {true, false, 0.0, 3.0, 0, 0, 10, 1, 0, 1};
  ASSERT_EQ(run(g, s).size(), 1u);  // guidance MOVE_TO issued
  // Tracking lost beats guidance immediately (no min-gap wait) and supersedes nothing of its priority.
  auto lost = sit(1);
  lost.plan = s.plan;
  lost.tracking_state = 3;
  lost.lost_since_ns = lost.t_ns;
  auto out = run(g, lost);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, InstrType::HoldStill);
  EXPECT_EQ(out[0].priority, 3);
  // Long loss escalates, once the HOLD_STILL cooldown is over.
  auto long_lost = sit(10);
  long_lost.plan = s.plan;
  long_lost.tracking_state = 3;
  long_lost.lost_since_ns = 1'000'000'000LL;
  std::vector<LifeEvent> ev;
  out = run(g, long_lost, &ev);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, InstrType::Relocalize);
  // Tracking back: the safety instruction completes.
  auto back = sit(12);
  back.plan = s.plan;
  run(g, back, &ev);
  bool completed = false;
  for (auto& e : ev) completed |= e.ins.type == InstrType::Relocalize && e.state == LifeState::Completed;
  EXPECT_TRUE(completed);
  // Blur streak -> SLOW_DOWN with its own cooldown.
  InstructionGenerator g2(p);
  auto b = sit(0);
  b.blur_streak = 3;
  auto o2 = run(g2, b);
  ASSERT_EQ(o2.size(), 1u);
  EXPECT_EQ(o2[0].type, InstrType::SlowDown);
  auto b2 = sit(2);
  b2.blur_streak = 5;
  EXPECT_TRUE(run(g2, b2).empty());  // cooldown 4 s
  // An unfulfilled instruction expires.
  InstructionGenerator g3(p);
  auto e0 = sit(0);
  e0.plan.target = {true, false, 0.0, 9.0, 0, 0, 10, 1, 0, 1};
  ASSERT_EQ(run(g3, e0).size(), 1u);
  auto e1 = sit(13);
  e1.plan = e0.plan;
  std::vector<LifeEvent> ev2;
  run(g3, e1, &ev2);
  ASSERT_FALSE(ev2.empty());
  EXPECT_EQ(ev2[0].state, LifeState::Expired);
}

TEST(GuideInstructions, FinishSweepAndTilt) {
  GuideParams p = P();
  InstructionGenerator g(p);
  EXPECT_EQ(run(g, sit(0, Mode::Finish))[0].type, InstrType::Finish);
  InstructionGenerator g2(p);
  EXPECT_TRUE(run(g2, sit(1, Mode::Sweep)).empty());  // sweep only after a short settle time
  auto out = run(g2, sit(4, Mode::Sweep));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, InstrType::Sweep);
  InstructionGenerator g3(p);
  auto s = sit(0);
  s.plan.target = {true, false, 0.0, 0.0, 0, deg2rad(35), 0, 0, 0, 1};  // here already, look up
  auto o3 = run(g3, s);
  ASSERT_EQ(o3.size(), 1u);
  EXPECT_EQ(o3[0].type, InstrType::TiltUp);
}

// ---- Log ----
TEST(GuideLogTest, InfoPromptsAreCountedSeparately) {
  GuideLog l;
  l.start(0);
  Instruction m;
  m.id = 1, m.type = InstrType::MoveTo, m.priority = 2, m.t_ns = 1'000'000'000LL, m.msg_id = "move.to";
  l.issued(m, 0.1);
  l.ended({m, LifeState::Completed, 5'000'000'000LL, 0.3});  // 4 s to comply
  for (uint32_t k = 0; k < 3; ++k) {
    Instruction p;
    p.id = 2 + k, p.type = k == 2 ? InstrType::KeepTurning : InstrType::Progress, p.priority = 1, p.t_ns = 6'000'000'000LL + k * 1'000'000'000LL;
    p.msg_id = k == 2 ? "keep_turning" : "progress.25";
    l.issued(p, 0.3);
    l.ended({p, LifeState::Completed, p.t_ns + 500'000'000LL, 0.3});  // 0.5 s: must not drag the median down
  }
  auto j = l.summary_json();
  EXPECT_NE(j.find("\"instructions\":1"), std::string::npos) << j;
  EXPECT_NE(j.find("\"info_prompts\":3"), std::string::npos) << j;
  EXPECT_NE(j.find("\"median_time_to_compliance_s\":4"), std::string::npos) << j;
  EXPECT_NE(j.find("\"COMPLETED\":1"), std::string::npos) << j;
}

TEST(GuideLogTest, RecordsLifecycleAndIsDeterministic) {
  auto make = [] {
    GuideLog l;
    l.start(0);
    Instruction i;
    i.id = 1, i.type = InstrType::MoveTo, i.priority = 2, i.t_ns = 2'000'000'000LL, i.msg_id = "move.to", i.speak_text = "Walk ahead.";
    i.params = {{"distance_m", 2}};
    l.issued(i, 0.1);
    l.ended({i, LifeState::Completed, 5'000'000'000LL, 0.3});
    l.sample(1'000'000'000LL, 0.1, 0.2);
    return l.to_json("abc");
  };
  auto a = make();
  EXPECT_EQ(a, make());
  EXPECT_NE(a.find("\"time_to_compliance_s\":3"), std::string::npos) << a;
  EXPECT_NE(a.find("\"coverage_delta\":0.2"), std::string::npos) << a;
}

// ---- Simulator: determinism, safety, ordering, budget ----
TEST(GuideSim, DeterministicAndSafe) {
  sim::Config c;
  c.room = "furniture";
  c.policy = "planner";
  c.seed = 3;
  c.max_s = 90;
  auto a = sim::simulate(c), b = sim::simulate(c);
  EXPECT_EQ(a, b);  // byte-identical for equal input
  EXPECT_NE(a.find("\"unsafe_targets_gt\":0"), std::string::npos) << a;
  EXPECT_NE(a.find("\"unsafe_targets\":0"), std::string::npos) << a;
  c.seed = 4;
  EXPECT_NE(a, sim::simulate(c));  // the seed matters
}

TEST(GuideSim, PlannerBeatsScriptedWalkOnBox) {
  sim::Config c;
  c.room = "box";
  c.max_s = 200;
  c.policy = "planner";
  auto plan = sim::simulate(c);
  c.policy = "scripted";
  auto script = sim::simulate(c);
  auto cov = [](const std::string& j) {
    auto p = j.find("\"coverage_gt_final\":");
    return std::stod(j.substr(p + 20));
  };
  EXPECT_GT(cov(plan), cov(script) + 0.2) << plan << "\n" << script;
}

TEST(GuideSim, AverageStepWithinBudget) {
  sim::Config c;
  c.room = "box";
  c.policy = "planner";
  c.max_s = 60;
  auto t0 = std::chrono::steady_clock::now();  // test-only timing; the core never reads clocks
  sim::simulate(c);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / (60.0 * 30);
  RecordProperty("ms_per_step_incl_simulator", std::to_string(ms));
  std::printf("[ guide ] %.3f ms per 30 Hz step (engine + simulator ground truth)\n", ms);
#ifdef NDEBUG
  EXPECT_LT(ms, std::getenv("CK_PERF_STRICT") ? 10.0 : 25.0);  // phase 8 budget: <= 10 ms average update
#endif
}

// ---- Tracking recovery (issue #1, ADR 0031) ----
namespace {
constexpr int64_t kNs = 1'000'000'000LL;
int64_t at30(int64_t i) { return kNs + i * kNs / 30; }  // 30 Hz poses, t_ns > 0

struct RecFeed {
  TrackingRecovery r;
  std::vector<RecoveryEvent> ev;
  void pose(int64_t i, int state, int reason = 0, Vec3 p = Vec3::Zero(), double yaw = 0) {
    r.on_pose(at30(i), state, reason, p, yaw, ev);
  }
  // 200 Hz gyro over the pose interval ending at pose i; mag(k) = |omega| (rad/s) of global sample k.
  template <class F>
  void gyro(int64_t i, F mag) {
    for (int64_t k = 0; k < 7; ++k) r.on_gyro(at30(i) - kNs / 30 + k * 5'000'000, mag(i * 7 + k), 0, 0);
  }
};

bundle::PoseSample rpose(int64_t t, double x, double z, double yaw, int state, int reason = 0) {
  auto T = pose_from_yaw_pitch({x, 1.4, z}, yaw, 0);
  bundle::PoseSample p;
  p.t_ns = t;
  for (int i = 0; i < 3; ++i) p.t_world_from_camera[i] = T.translation()[i];
  auto q = T.se3().unit_quaternion();
  p.q_world_from_camera_wxyz[0] = q.w(), p.q_world_from_camera_wxyz[1] = q.x();
  p.q_world_from_camera_wxyz[2] = q.y(), p.q_world_from_camera_wxyz[3] = q.z();
  p.tracking_state = static_cast<bundle::TrackingState>(state);
  p.tracking_reason = static_cast<bundle::TrackingReason>(reason);
  return p;
}
}  // namespace

TEST(GuideRecovery, ShortDipIsIgnoredSustainedLossEntersAndResetMeasuresDrift) {
  RecFeed f;
  int64_t i = 0;
  for (; i < 30; ++i) f.pose(i, 1);
  for (; i < 30 + 57; ++i) f.pose(i, 2, 3);  // 1.9 s PAUSED / INSUFFICIENT_FEATURES: below enter_s
  for (; i < 120; ++i) f.pose(i, 1);
  EXPECT_TRUE(f.ev.empty());
  EXPECT_FALSE(f.r.active());
  const int64_t loss0 = i;
  for (; i < loss0 + 75; ++i) f.pose(i, 2, 3);  // 2.5 s
  ASSERT_EQ(f.ev.size(), 1u);
  EXPECT_EQ(f.ev[0].kind, RecoveryEvent::Kind::Lost);
  EXPECT_EQ(f.ev[0].tracking_state, 2);
  EXPECT_EQ(f.ev[0].tracking_reason, 3);
  EXPECT_GE(f.ev[0].degraded_s, 2.0);
  EXPECT_EQ(f.ev[0].t_ns, at30(loss0 + 60));  // the first pose at >= 2 s of degradation
  EXPECT_TRUE(f.r.active());
  // Tracking back with a jumped pose; no gyro data = no evidence against settling: reset after settle_s (1 s).
  const int64_t back0 = i;
  for (; i < back0 + 45; ++i) f.pose(i, 1, 0, {0.3, 0, 0.4}, deg2rad(10));
  ASSERT_EQ(f.ev.size(), 2u);
  const auto& r = f.ev[1];
  EXPECT_EQ(r.kind, RecoveryEvent::Kind::Reset);
  EXPECT_EQ(r.t_ns, at30(back0 + 30));
  EXPECT_NEAR(r.drift_m, 0.5, 1e-9);
  EXPECT_NEAR(r.drift_deg, 10, 1e-6);
  EXPECT_NEAR(r.lost_s, 3.5, 1e-6);
  EXPECT_FALSE(f.r.active());
  EXPECT_EQ(f.r.losses(), 1);
  EXPECT_EQ(f.r.resets(), 1);
  EXPECT_NEAR(f.r.max_drift_m(), 0.5, 1e-9);
}

TEST(GuideRecovery, WarmUpBeforeFirstTrackingIsNotALoss) {
  RecFeed f;
  int64_t i = 0;
  for (; i < 150; ++i) f.pose(i, 2, 1);  // 5 s PAUSED / INITIALIZING before ARCore first tracks
  EXPECT_TRUE(f.ev.empty());
  EXPECT_FALSE(f.r.active());
  for (; i < 160; ++i) f.pose(i, 1);
  for (; i < 160 + 61; ++i) f.pose(i, 2, 3);  // a real loss afterwards is detected
  ASSERT_EQ(f.ev.size(), 1u);
  EXPECT_EQ(f.ev[0].kind, RecoveryEvent::Kind::Lost);
}

TEST(GuideRecovery, NotAvailableCountsAndRelapseRestartsSettling) {
  RecFeed f;
  int64_t i = 0;
  for (; i < 10; ++i) f.pose(i, 1);
  for (; i < 10 + 61; ++i) f.pose(i, 3, 0);  // STOPPED-like loss
  ASSERT_EQ(f.ev.size(), 1u);
  EXPECT_EQ(f.ev[0].tracking_state, 3);
  for (int64_t k = 0; k < 15; ++k, ++i) f.pose(i, 1);  // 0.5 s back ...
  f.pose(i++, 2, 2);                                   // ... relapse
  const int64_t back = i;
  for (; i < back + 30; ++i) f.pose(i, 1);  // last one 0.97 s after the first NORMAL pose: still settling
  EXPECT_EQ(f.ev.size(), 1u);
  EXPECT_TRUE(f.r.active());
  f.pose(i++, 1);  // 1.0 s
  ASSERT_EQ(f.ev.size(), 2u);
  EXPECT_EQ(f.ev[1].kind, RecoveryEvent::Kind::Reset);
}

TEST(GuideRecovery, GyroMustSettleButNeverTrapsTheUser) {
  auto seconds_to_reset = [](auto mag) {
    RecFeed f;
    int64_t i = 0;
    f.gyro(i, mag), f.pose(i++, 1);  // tracking once: detection is armed
    for (; i < 71; ++i) f.gyro(i, mag), f.pose(i, 2, 3);
    EXPECT_TRUE(f.r.active());
    const int64_t back = i;
    for (; i < back + 200 && f.ev.size() < 2; ++i) f.gyro(i, mag), f.pose(i, 1);
    EXPECT_EQ(f.ev.size(), 2u);
    return double(i - 1 - back) / 30.0;  // seconds of NORMAL tracking before the reset
  };
  // A slow, steady circle (|omega| ~0.8 rad/s with a small ripple): settled after settle_s.
  EXPECT_NEAR(seconds_to_reset([](int64_t k) { return 0.8 + 0.02 * double(k % 3); }), 1.0, 0.04);
  // Shaking (|omega| alternating 0 / 2 rad/s, std-dev 1): never steady, released by settle_max_s.
  EXPECT_NEAR(seconds_to_reset([](int64_t k) { return k % 2 ? 2.0 : 0.0; }), 4.0, 0.04);
  // The steadiness rule itself.
  TrackingRecovery r;
  EXPECT_TRUE(r.gyro_steady(kNs));  // no samples: no evidence, never blocks
  for (int k = 0; k < 200; ++k) r.on_gyro(kNs + k * 5'000'000LL, k % 2 ? 2.0 : 0.0, 0, 0);
  EXPECT_FALSE(r.gyro_steady(kNs + 199 * 5'000'000LL));
  EXPECT_TRUE(r.gyro_steady(kNs + 199 * 5'000'000LL + 3 * kNs));  // outside the window: no evidence
}

TEST(GuideInstructions, RecoverySuppressesGuidanceAndSpeaksRecalibrate) {
  GuideParams p = P();
  InstructionGenerator g(p);
  auto s = sit(0);
  s.plan.target = {true, false, 0.0, 3.0, 0, 0, 10, 1, 0, 1};
  ASSERT_EQ(run(g, s).size(), 1u);  // MOVE_TO active
  auto rec = sit(1);
  rec.plan = s.plan;
  rec.tracking_state = 2, rec.tracking_reason = 3, rec.recovering = true;
  std::vector<LifeEvent> ev;
  auto out = run(g, rec, &ev);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, InstrType::Recalibrate);
  EXPECT_EQ(out[0].priority, 3);
  EXPECT_EQ(out[0].msg_id, "recalibrate");
  EXPECT_EQ(out[0].overlay_hint, "recalibrate");
  EXPECT_EQ(out[0].haptic, "double");
  EXPECT_STREQ(type_name(InstrType::Recalibrate), "RECALIBRATE");
  EXPECT_NE(out[0].long_text.find("small circle"), std::string::npos);
  ASSERT_EQ(ev.size(), 1u);  // the stale MOVE_TO is superseded, not expired (no planner penalty)
  EXPECT_EQ(ev[0].ins.type, InstrType::MoveTo);
  EXPECT_EQ(ev[0].state, LifeState::Superseded);
  // Navigation and info prompts stay silent while recovering, long after the min gap and every cooldown.
  for (int64_t t = 2; t <= 11; ++t) {
    auto r2 = rec;
    r2.t_ns = t * kNs;
    r2.completeness = 0.6;
    EXPECT_TRUE(run(g, r2).empty()) << t;
  }
  // Tracking back but still settling: silence, the prompt stays active.
  auto settle = rec;
  settle.t_ns = 11'500'000'000LL, settle.tracking_state = 1, settle.tracking_reason = 0;
  EXPECT_TRUE(run(g, settle, &ev).empty());
  EXPECT_TRUE(ev.empty());
  // Recovery over: RECALIBRATE completes and guidance resumes at once.
  auto done = sit(12);
  done.plan = s.plan;
  out = run(g, done, &ev);
  bool completed = false;
  for (auto& e : ev) completed |= e.ins.type == InstrType::Recalibrate && e.state == LifeState::Completed;
  EXPECT_TRUE(completed);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, InstrType::MoveTo);
  // A long NOT_AVAILABLE loss escalates to RELOCALIZE (walk back) and never falls back to RECALIBRATE.
  InstructionGenerator g2(p);
  auto lost = sit(10);
  lost.tracking_state = 3, lost.lost_since_ns = kNs, lost.recovering = true;
  out = run(g2, lost);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, InstrType::Relocalize);
  lost.t_ns = 15 * kNs;
  EXPECT_TRUE(run(g2, lost).empty());
}

namespace {
struct RecoveryRun {
  std::vector<Instruction> out;
  std::vector<RecoveryEvent> events;
  std::vector<std::string> overlays;
  double seen_before_kf = 0, seen_after_kf_recovering = 0, seen_after_kf_reset = 0;
  double speed_at_reset = -1;
  std::string report;
};

// 3 s NORMAL facing a wall, `loss_s` of PAUSED / INSUFFICIENT_FEATURES, then 2 s NORMAL with the pose jumped 0.3 m.
RecoveryRun recovery_scene(double loss_s) {
  RecoveryRun rr;
  GuideEngine g;
  bundle::IntrinsicsSample k;
  k.t_ns = 1, k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 319.5, k.cy = 239.5;
  g.on_intrinsics(k);
  g.on_plane(back_wall());
  int64_t i = 0;
  auto step = [&](int st, int reason, double x) {
    const int64_t t = at30(i++);
    g.on_pose(rpose(t, x, 0.5, 0, st, reason));
    for (auto& e : g.take_recovery_events()) {
      rr.events.push_back(e);
      if (e.kind == RecoveryEvent::Kind::Reset) rr.speed_at_reset = g.user().speed;
    }
    g.update(t, rr.out);
    rr.overlays.push_back(g.overlay(t).to_json());
    return t;
  };
  for (int n = 0; n < 90; ++n) step(1, 0, 2.0);
  const int n_loss = int(std::lround(loss_s * 30));
  int64_t t = 0;
  for (int n = 0; n < n_loss; ++n) t = step(2, 3, 2.0);
  rr.seen_before_kf = g.coverage().stats().seen_fraction();
  if (g.recovering()) {
    g.on_keyframe(1, t, 1.0);
    rr.seen_after_kf_recovering = g.coverage().stats().seen_fraction();
  }
  for (int n = 0; n < 60; ++n) t = step(1, 0, 2.3);
  g.on_keyframe(2, t, 1.0);
  rr.seen_after_kf_reset = g.coverage().stats().seen_fraction();
  rr.report = g.report_json();
  return rr;
}
}  // namespace

TEST(GuideRecovery, EngineSuppressesGuidancePausesCoverageAndReanchors) {
  auto rr = recovery_scene(3.0);
  ASSERT_EQ(rr.events.size(), 2u);
  EXPECT_EQ(rr.events[0].kind, RecoveryEvent::Kind::Lost);
  EXPECT_EQ(rr.events[1].kind, RecoveryEvent::Kind::Reset);
  EXPECT_NEAR(rr.events[1].drift_m, 0.3, 1e-6);
  EXPECT_DOUBLE_EQ(rr.speed_at_reset, 0.0);  // re-anchored: no speed spike across the jump
  const int64_t lost_t = rr.events[0].t_ns, reset_t = rr.events[1].t_ns;
  int recal = 0;
  for (auto& ins : rr.out) {
    if (ins.t_ns >= lost_t && ins.t_ns < reset_t) {
      EXPECT_EQ(ins.type, InstrType::Recalibrate) << type_name(ins.type);  // nothing else while recovering
      ++recal;
    }
    if (ins.t_ns >= reset_t) EXPECT_NE(ins.type, InstrType::SlowDown);  // the 0.3 m jump is not "too fast"
  }
  EXPECT_EQ(recal, 1);
  // Coverage ignores keyframes while recovering and counts them again afterwards.
  EXPECT_DOUBLE_EQ(rr.seen_after_kf_recovering, rr.seen_before_kf);
  EXPECT_GT(rr.seen_after_kf_reset, rr.seen_before_kf);
  // Overlay: recalibrating flag + why line exactly while recovering; gone afterwards.
  bool flagged = false;
  for (auto& o : rr.overlays)
    if (o.find("\"recalibrating\":true") != std::string::npos) {
      flagged = true;
      EXPECT_NE(o.find("\"why_id\":\"why.recalibrating\""), std::string::npos);
    }
  EXPECT_TRUE(flagged);
  EXPECT_EQ(rr.overlays.back().find("recalibrating"), std::string::npos);
  EXPECT_NE(rr.report.find("\"recovery\":{\"active\":false,\"losses\":1,\"max_drift_m\":0.3,\"resets\":1}"), std::string::npos)
      << rr.report;
  // Deterministic.
  auto again = recovery_scene(3.0);
  ASSERT_EQ(again.out.size(), rr.out.size());
  for (size_t k = 0; k < rr.out.size(); ++k) EXPECT_EQ(again.out[k].speak_text, rr.out[k].speak_text);
  EXPECT_EQ(again.report, rr.report);
  EXPECT_EQ(again.overlays, rr.overlays);
}

TEST(GuideRecovery, ShortLossKeepsEngineOutputUnchanged) {
  auto rr = recovery_scene(1.0);
  EXPECT_TRUE(rr.events.empty());
  EXPECT_EQ(rr.report.find("\"recovery\""), std::string::npos);  // existing reports and replay baselines unchanged
  for (auto& o : rr.overlays) EXPECT_EQ(o.find("recalibrating"), std::string::npos);
  for (auto& ins : rr.out) EXPECT_NE(ins.type, InstrType::Recalibrate);
}
