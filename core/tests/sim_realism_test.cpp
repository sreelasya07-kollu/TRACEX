// T4 sim realism (ADR 0029): the "arcore" plane mode and the furnished4 / furnishedL rooms. Fragment counts follow
// from the room geometry; ids, re-ids, merges and jitter come from the plane RNG, so equal seeds give equal reports.
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "capturekit/guide/sim.hpp"

using namespace capturekit::guide;

namespace {

sim::Config cfg(const std::string& room, const std::string& policy, uint64_t seed, double max_s) {
  sim::Config c;
  c.room = room, c.policy = policy, c.seed = seed, c.max_s = max_s;
  return c;
}

/// The report's "sim_planes" object ("" when absent).
std::string planes_block(const std::string& r) {
  const auto a = r.find("\"sim_planes\":{");
  if (a == std::string::npos) return "";
  return r.substr(a, r.find('}', a) - a + 1);
}

double num(const std::string& block, const std::string& key) {
  const auto a = block.find("\"" + key + "\":");
  EXPECT_NE(a, std::string::npos) << key;
  if (a == std::string::npos) return -1;
  return std::strtod(block.c_str() + a + key.size() + 3, nullptr);
}

/// Simulates the configs in parallel (simulate() is re-entrant).
std::vector<std::string> run_all(const std::vector<sim::Config>& cs) {
  std::vector<std::string> out(cs.size());
  std::vector<std::thread> th;
  for (size_t k = 0; k < cs.size(); ++k) th.emplace_back([&, k] { out[k] = sim::simulate(cs[k]); });
  for (auto& t : th) t.join();
  return out;
}

}  // namespace

TEST(SimRealism, RoomsDefaultToArcoreAndOthersStayIdeal) {
  EXPECT_TRUE(sim::make_room("furnished4").arcore_planes);
  EXPECT_TRUE(sim::make_room("furnishedL").arcore_planes);
  for (const char* n : {"box", "L", "furniture", "objects", "door_window"}) EXPECT_FALSE(sim::make_room(n).arcore_planes) << n;
  const auto f4 = sim::make_room("furnished4"), fl = sim::make_room("furnishedL");
  EXPECT_EQ(f4.poly.size(), 4u);
  EXPECT_EQ(fl.poly.size(), 6u);
  EXPECT_EQ(f4.boxes.size(), 3u);  // sofa, wardrobe, bed
  EXPECT_EQ(fl.boxes.size(), 3u);
}

TEST(SimRealism, FragmentCountsFollowFurnitureAgainstWalls) {
  const auto r = run_all({cfg("furnished4", "scripted", 1, 150), cfg("furnishedL", "scripted", 1, 150)});
  const std::string a = planes_block(r[0]), b = planes_block(r[1]);
  ASSERT_FALSE(a.empty());
  ASSERT_FALSE(b.empty());
  // furnished4: sofa splits wall 0, wardrobe wall 1, bed wall 3; wall 2 is textureless (no plane at all)
  EXPECT_EQ(num(a, "true_walls"), 4);
  EXPECT_EQ(num(a, "textureless_walls"), 1);
  EXPECT_EQ(num(a, "fragments"), 6);
  EXPECT_NE(a.find("\"fragments_by_wall\":[2,2,0,2]"), std::string::npos) << a;
  // furnishedL: sofa wall 0, bed wall 1, wardrobe wall 5 (its 0.2 m sliver is too small for a plane); wall 4 textureless
  EXPECT_EQ(num(b, "true_walls"), 6);
  EXPECT_EQ(num(b, "fragments"), 7);
  EXPECT_NE(b.find("\"fragments_by_wall\":[2,2,1,1,0,1]"), std::string::npos) << b;
  for (const std::string* s : {&a, &b}) {
    EXPECT_GE(num(*s, "tracks"), num(*s, "fragments")) << *s;  // + pre-merge halves
    EXPECT_GE(num(*s, "wall_plane_ids"), num(*s, "fragments")) << *s;  // more plane ids than true walls
    EXPECT_GE(num(*s, "jitter_deg_min"), 2.0) << *s;
    EXPECT_LE(num(*s, "jitter_deg_max"), 4.0) << *s;
    EXPECT_GE(num(*s, "max_live_wall_planes"), 4) << *s;
  }
}

TEST(SimRealism, ReidsAndSubsumptionHappen) {
  std::vector<sim::Config> cs;
  for (uint64_t s = 0; s < 4; ++s) cs.push_back(cfg(s % 2 ? "furnishedL" : "furnished4", "scripted", s, 300));
  const auto r = run_all(cs);
  double reids = 0, subsumed = 0, halves = 0;
  for (const auto& rep : r) {
    const std::string p = planes_block(rep);
    ASSERT_FALSE(p.empty());
    reids += num(p, "reids"), subsumed += num(p, "subsumed");
    halves += num(p, "tracks") - num(p, "fragments");
    EXPECT_LE(num(p, "wall_plane_ids"), num(p, "tracks") + num(p, "reids")) << p;  // one id per track + one per re-id
  }
  EXPECT_GT(reids, 0);
  EXPECT_GT(halves, 0);  // some fragment started as two planes
  EXPECT_GT(subsumed, 0);  // and they merged (one id went stale)
}

TEST(SimRealism, DeterministicPerSeed) {
  const auto r = run_all({cfg("furnished4", "scripted", 3, 120), cfg("furnished4", "scripted", 3, 120),
                          cfg("furnished4", "scripted", 4, 120), cfg("furnishedL", "planner", 2, 40),
                          cfg("furnishedL", "planner", 2, 40)});
  EXPECT_EQ(r[0], r[1]);
  EXPECT_NE(r[0], r[2]);  // the seed matters
  EXPECT_EQ(r[3], r[4]);  // the engine consumes the arcore planes deterministically
  EXPECT_NE(r[3].find("\"guide\":"), std::string::npos);
  EXPECT_FALSE(planes_block(r[3]).empty());
}

TEST(SimRealism, IdealModeHasNoPlanesBlock) {
  auto c = cfg("furnished4", "scripted", 1, 30);
  c.planes = "ideal";
  const auto r = run_all({c, cfg("box", "scripted", 1, 30)});
  EXPECT_TRUE(planes_block(r[0]).empty());
  EXPECT_TRUE(planes_block(r[1]).empty());
  auto f = cfg("box", "scripted", 1, 30);
  f.planes = "arcore";  // any room can opt in; the box has no furniture: one fragment per wall
  const std::string p = planes_block(sim::simulate(f));
  EXPECT_NE(p.find("\"fragments_by_wall\":[1,1,1,1]"), std::string::npos) << p;
}
