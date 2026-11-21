// Phase 9 room understanding: stage-by-stage tests (step 1: types + SurfaceClassifier; step 2: SparsePointStore +
// OpeningDetector).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "capturekit/guide/coverage.hpp"
#include "capturekit/guide/engine.hpp"
#include "capturekit/guide/landmark.hpp"
#include "capturekit/guide/live_map.hpp"
#include "capturekit/guide/nav_grid.hpp"
#include "capturekit/guide/object_lifter.hpp"
#include "capturekit/guide/opening_detector.hpp"
#include "capturekit/guide/point_store.hpp"
#include "capturekit/guide/room_shell.hpp"
#include "capturekit/guide/sim.hpp"
#include "capturekit/guide/surface_classifier.hpp"
#include "capturekit/guide/understanding.hpp"
#include "capturekit/util/json.hpp"

using namespace capturekit;
using namespace capturekit::guide;

namespace {

bundle::PlaneRecord plane(const std::string& id, const std::string& kind, Vec3 c, Vec3 n, Vec3 u, double eu, double ev) {
  bundle::PlaneRecord p;
  p.id = id, p.kind = kind;
  for (int i = 0; i < 3; ++i) p.center[i] = c[i], p.normal[i] = n[i], p.axis_u[i] = u[i];
  p.extent_u = eu, p.extent_v = ev;
  return p;
}

PlaneGeom geom(Vec3 c, Vec3 n, double area) {
  PlaneGeom g;
  g.c = c, g.n = n, g.area = area;
  return g;
}

struct Truth {
  bundle::PlaneRecord rec;  // platform label as ARCore would give it
  std::string kind;         // what it really is
};

/// Ground-truth planes of a sim room. Platform labels mimic ARCore: every upward plane (furniture tops
/// included) says "floor", every vertical one "wall" (even picture frames).
std::vector<Truth> room_planes(const std::string& name, bool clutter) {
  auto room = sim::make_room(name);
  std::vector<Truth> out;
  const auto& P = room.poly;
  for (size_t i = 0; i < P.size(); ++i) {
    auto [x0, z0] = P[i];
    auto [x1, z1] = P[(i + 1) % P.size()];
    double L = std::hypot(x1 - x0, z1 - z0);
    Vec3 u((x1 - x0) / L, 0, (z1 - z0) / L), n(-(z1 - z0) / L, 0, (x1 - x0) / L);
    Vec3 c(0.5 * (x0 + x1), 0.5 * room.height, 0.5 * (z0 + z1));
    out.push_back({plane("w" + std::to_string(i), "wall", c, n, u, L, room.height), "wall"});
  }
  int k = 0;
  for (auto& r : room.rects) {
    Vec3 cf(0.5 * (r[0] + r[2]), 0.0, 0.5 * (r[1] + r[3])), cc = cf + Vec3(0, room.height, 0);
    double ex = r[2] - r[0], ez = r[3] - r[1];
    out.push_back({plane("f" + std::to_string(k), "floor", cf, {0, 1, 0}, {1, 0, 0}, ex, ez), "floor"});
    out.push_back({plane("c" + std::to_string(k), "ceiling", cc, {0, -1, 0}, {1, 0, 0}, ex, ez), "ceiling"});
    ++k;
  }
  k = 0;
  for (auto& b : room.boxes) {
    Vec3 c(0.5 * (b.x0 + b.x1), b.h, 0.5 * (b.z0 + b.z1));
    // 2.0 m is not above the 2.0 m ceiling threshold, and above the 1.2 m table limit: "other".
    out.push_back({plane("t" + std::to_string(k++), "floor", c, {0, 1, 0}, {1, 0, 0}, b.x1 - b.x0, b.z1 - b.z0),
                   b.h <= 1.2 ? "table" : "other"});
  }
  if (clutter) {
    out.push_back({plane("seat", "floor", {1.5, 0.45, 1.5}, {0, 1, 0}, {1, 0, 0}, 0.45, 0.45), "table"});
    out.push_back({plane("shelf", "floor", {0.4, 1.6, 2.0}, {0, 1, 0}, {1, 0, 0}, 0.9, 0.3), "other"});
    out.push_back({plane("frame", "wall", {2.0, 1.5, 0.35}, {0, 0, 1}, {1, 0, 0}, 0.4, 0.3), "other"});
    out.push_back({plane("tilt", "other", {2.0, 1.0, 3.0}, Vec3(0, 0.7, 0.7).normalized(), {1, 0, 0}, 1.0, 1.0), "other"});
  }
  return out;
}

struct Accuracy {
  int n = 0, ok = 0;
  double value() const { return n ? double(ok) / n : 0.0; }
};

/// Feeds every plane three times (a plane grows/re-arrives at 1 Hz), `adversarial` = furniture tops first
/// and the floor last, and counts final kinds against the truth.
Accuracy run_room(const std::string& name, bool clutter, bool adversarial, bool classify) {
  auto planes = room_planes(name, clutter);
  if (adversarial) {  // tops/clutter first, then walls, floor and ceiling last
    std::stable_sort(planes.begin(), planes.end(), [](const Truth& a, const Truth& b) {
      auto rank = [](const Truth& t) { return t.kind == "table" || t.kind == "other" ? 0 : t.kind == "wall" ? 1 : 2; };
      return rank(a) < rank(b);
    });
  }
  LiveMap m;
  m.set_classify(classify);
  m.add_camera({2.0, 1.4, 2.0});
  std::map<std::string, std::string> canon;
  for (int round = 0; round < 3; ++round)
    for (auto& t : planes) canon[t.rec.id] = m.add_plane(t.rec);
  Accuracy a;
  for (auto& t : planes) {
    ++a.n;
    a.ok += m.planes().at(canon[t.rec.id]).kind == t.kind;
  }
  return a;
}

}  // namespace

// ---- types ----
TEST(Understanding, ObjectClassNamesRoundTrip) {
  for (int i = 0; i < int(ObjectClass::Count_); ++i) {
    auto c = ObjectClass(i);
    EXPECT_EQ(object_class_from_name(object_class_name(c)), c);
  }
  EXPECT_EQ(object_class_from_name("sofa-bed-9000"), ObjectClass::Unknown);
  EXPECT_STREQ(object_class_name(ObjectClass(99)), "unknown");
  EXPECT_TRUE(object_class_wall_mounted(ObjectClass::Tv));
  EXPECT_FALSE(object_class_wall_mounted(ObjectClass::Chair));
}

// Class ids are recorded in bundles (/ar/detections, /guide/labels) and mapped by the shells (LabelMap.kt): they
// are append-only (ADR 0030). This pins every id so a reorder or insertion fails here, not in a Part 2 reader.
TEST(Understanding, ObjectClassIdsAreFrozen) {
  const std::vector<std::pair<int, const char*>> ids = {
      {0, "unknown"}, {1, "chair"}, {2, "couch"}, {3, "bed"}, {4, "dining_table"}, {5, "tv"}, {6, "potted_plant"},
      {7, "laptop"}, {8, "bench"}, {9, "toilet"}, {10, "sink"}, {11, "refrigerator"}, {12, "oven"}, {13, "desk"},
      {14, "shelf"}, {15, "cabinet"}, {16, "lamp"}};
  ASSERT_EQ(int(ObjectClass::Count_), int(ids.size()));
  for (auto& [id, name] : ids) EXPECT_STREQ(object_class_name(ObjectClass(id)), name) << id;
  EXPECT_FALSE(object_class_wall_mounted(ObjectClass::Refrigerator));
  EXPECT_FALSE(object_class_wall_mounted(ObjectClass::Lamp));
}

// Every real class has its own prior (never the generic default) that fits under the lifter's height cut, and its
// own spoken phrase (never the generic "furniture").
TEST(Understanding, EveryObjectClassHasAPriorAndAPhrase) {
  const Vec3 generic = ObjectLifter::class_prior(ObjectClass::Unknown);
  const LifterParams lp;
  for (int i = 1; i < int(ObjectClass::Count_); ++i) {
    const auto c = ObjectClass(i);
    const Vec3 pr = ObjectLifter::class_prior(c);
    EXPECT_GT(pr.x(), 0) << i;
    EXPECT_GT(pr.y(), 0) << i;
    EXPECT_GT(pr.z(), 0) << i;
    EXPECT_LE(pr.y(), lp.max_h) << i;
    EXPECT_FALSE(pr.isApprox(generic)) << object_class_name(c) << " falls back to the generic prior";
    EXPECT_STRNE(LandmarkLabeler::class_phrase(c), "furniture") << object_class_name(c);
  }
}

// ---- SurfaceClassifier rules ----
TEST(Understanding, ClassifierRules) {
  SurfaceContext fl;
  fl.has_floor_ref = true, fl.floor_y = 0.0;
  struct Row {
    const char* what;
    PlaneGeom g;
    const char* kind;
  };
  const Vec3 up(0, 1, 0), down(0, -1, 0), wallN(0, 0, -1);
  std::vector<Row> rows = {
      {"floor", geom({2, 0.02, 2}, up, 16), "floor"},
      {"below floor", geom({2, -0.3, 2}, up, 16), "floor"},
      {"table top", geom({2, 0.75, 2}, up, 1.0), "table"},
      {"seat", geom({2, 0.45, 2}, up, 0.2), "table"},
      {"high shelf", geom({2, 1.6, 2}, up, 0.5), "other"},
      {"ceiling", geom({2, 2.5, 2}, down, 16), "ceiling"},
      {"ceiling, sign ignored", geom({2, 2.5, 2}, up, 16), "ceiling"},
      {"wall", geom({2, 1.25, 4}, wallN, 10), "wall"},
      {"small vertical", geom({2, 1.0, 4}, wallN, 0.3), "other"},
      {"wall at area limit", geom({2, 1.0, 4}, wallN, 0.5), "wall"},
      {"tilted 45 deg", geom({2, 1, 2}, Vec3(0, 1, 1).normalized(), 4), "other"},
      {"zero area", geom({2, 0, 2}, up, 0), "other"},
  };
  for (auto& r : rows) {
    auto l = SurfaceClassifier::classify(r.g, fl);
    EXPECT_EQ(l.kind, r.kind) << r.what;
    EXPECT_GE(l.confidence, 0.0) << r.what;
    EXPECT_LE(l.confidence, 1.0) << r.what;
  }
  // No floor reference yet: the first horizontal plane is the floor; above the cameras it is a ceiling instead.
  SurfaceContext none;
  EXPECT_EQ(SurfaceClassifier::classify(geom({2, 0.8, 2}, up, 1.0), none).kind, "floor");
  none.has_cam = true, none.cam_y = 1.4;
  EXPECT_EQ(SurfaceClassifier::classify(geom({2, 2.5, 2}, up, 16), none).kind, "ceiling");
  EXPECT_EQ(SurfaceClassifier::classify(geom({2, 0.0, 2}, up, 16), none).kind, "floor");
  // Determinism and confidence ordering: a floor in plain view beats a borderline plane.
  auto a = SurfaceClassifier::classify(geom({2, 0.0, 2}, up, 16), fl);
  auto b = SurfaceClassifier::classify(geom({2, 0.14, 2}, up, 1.0), fl);
  EXPECT_EQ(a.kind, "floor");
  EXPECT_EQ(b.kind, "floor");
  EXPECT_GT(a.confidence, b.confidence);
  EXPECT_EQ(SurfaceClassifier::classify(geom({2, 0.75, 2}, up, 1.0), fl).confidence,
            SurfaceClassifier::classify(geom({2, 0.75, 2}, up, 1.0), fl).confidence);
}

TEST(Understanding, ClassifierHysteresis) {
  SurfaceClassifier c;
  SurfaceContext fl;
  fl.has_floor_ref = true, fl.floor_y = 0;
  const Vec3 up(0, 1, 0);
  auto floorish = geom({2, 0.05, 2}, up, 4), tableish = geom({2, 0.75, 2}, up, 4);
  EXPECT_EQ(c.update("p", floorish, fl).kind, "floor");  // first observation is published at once
  EXPECT_EQ(c.update("p", tableish, fl).kind, "floor");  // one disagreeing update is not enough
  EXPECT_EQ(c.update("p", floorish, fl).kind, "floor");  // flicker back resets the streak
  EXPECT_EQ(c.update("p", tableish, fl).kind, "floor");
  EXPECT_EQ(c.update("p", tableish, fl).kind, "table");  // two consecutive: switch
  EXPECT_EQ(c.update("p", floorish, fl).kind, "table");
  EXPECT_EQ(c.update("p", tableish, fl).kind, "table");
  // Independent planes do not share state; forget() restarts one.
  EXPECT_EQ(c.update("q", tableish, fl).kind, "table");
  c.forget("p");
  EXPECT_EQ(c.update("p", floorish, fl).kind, "floor");
}

// ---- accuracy over the sim rooms (ARCore-style labels: every upward plane says "floor") ----
TEST(Understanding, ClassifierAccuracyTable) {
  struct Row {
    const char* room;
    bool clutter;
  };
  const Row rows[] = {{"box", false}, {"L", false}, {"furniture", false}, {"furniture", true}};
  std::cout << "\n  Understanding.ClassifierAccuracyTable (synthetic planes, 3 updates each, cameras at 1.4 m)\n"
            << "  room            order        classifier        platform label\n";
  for (auto& r : rows)
    for (bool adv : {false, true}) {
      Accuracy on = run_room(r.room, r.clutter, adv, true), off = run_room(r.room, r.clutter, adv, false);
      char line[160];
      std::snprintf(line, sizeof line, "  %-9s%-6s %-12s %2d/%-2d %5.1f%%    %2d/%-2d %5.1f%%", r.room, r.clutter ? "+clut" : "",
                    adv ? "adversarial" : "natural", on.ok, on.n, 100 * on.value(), off.ok, off.n, 100 * off.value());
      std::cout << line << "\n";
      EXPECT_GE(on.value(), 0.95) << r.room << " clutter=" << r.clutter << " adversarial=" << adv;
      EXPECT_GE(on.ok, off.ok);
    }
}

TEST(Understanding, ClassifierBoxAndLAreExact) {
  for (const char* room : {"box", "L"})
    for (bool adv : {false, true}) {
      Accuracy a = run_room(room, false, adv, true);
      EXPECT_EQ(a.ok, a.n) << room;
    }
}

// ---- table tops must stop being coverage targets ----
TEST(Understanding, TableTopsNotCoveredAsFloor) {
  auto tops = [] {
    std::vector<bundle::PlaneRecord> v;
    v.push_back(plane("t1", "floor", {1.0, 0.75, 1.0}, {0, 1, 0}, {1, 0, 0}, 1.2, 0.8));
    v.push_back(plane("t2", "floor", {3.0, 0.45, 3.0}, {0, 1, 0}, {1, 0, 0}, 1.0, 1.0));
    return v;
  };
  auto floor_rec = plane("f", "floor", {2, 0.02, 2}, {0, 1, 0}, {1, 0, 0}, 4, 4);
  GuideParams p;
  auto valid_cells = [&](bool classify, bool with_tops, bool tops_first) {
    LiveMap m;
    m.set_classify(classify);
    m.add_camera({2.0, 1.4, 2.0});
    Coverage cov(p);
    for (int round = 0; round < 3; ++round) {
      if (tops_first && with_tops)
        for (auto& t : tops()) m.add_plane(t);
      m.add_plane(floor_rec);
      if (!tops_first && with_tops)
        for (auto& t : tops()) m.add_plane(t);
      cov.sync(m);  // the engine syncs after every plane
    }
    if (classify && with_tops)
      for (auto& [id, pl] : m.planes()) {
        if (id == "f") continue;
        EXPECT_EQ(pl.kind, "table") << id;
        EXPECT_EQ(pl.src_kind, "floor") << id;  // platform label kept as a hint
      }
    return cov.stats().valid;
  };
  const size_t floor_only = valid_cells(true, false, false);
  ASSERT_GT(floor_only, 1000u);
  EXPECT_EQ(valid_cells(true, true, false), floor_only);
  EXPECT_EQ(valid_cells(true, true, true), floor_only);        // tops first: relabelled once the floor shows up
  EXPECT_GT(valid_cells(false, true, false), floor_only + 100);  // flag off keeps the Phase 8 behaviour (counted as floor)
}

TEST(Understanding, ClassifierKeepsLevelsAndTable) {
  LiveMap m;
  m.set_classify(true);
  m.add_camera({2.0, 1.4, 2.0});
  m.add_plane(plane("f", "floor", {2, 0.0, 2}, {0, 1, 0}, {1, 0, 0}, 4, 4));
  m.add_plane(plane("t", "floor", {1, 0.75, 1}, {0, 1, 0}, {1, 0, 0}, 1, 1));
  for (int i = 0; i < 4; ++i) m.add_plane(plane("t", "floor", {1, 0.75, 1}, {0, 1, 0}, {1, 0, 0}, 1, 1));
  EXPECT_EQ(m.planes().at("t").kind, "table");
  EXPECT_EQ(m.planes().at("f").kind, "floor");
  EXPECT_TRUE(m.has_floor());
  EXPECT_NEAR(m.floor_y(), 0.0, 1e-9);
}

// ---- flag off is a no-op ----
TEST(Understanding, FlagOffIsNoOp) {
  // Config hash: the new switch is invisible while 0, and distinguishes the config when on.
  GuideParams def;
  EXPECT_EQ(def.to_json().find("understanding"), std::string::npos);
  auto off = GuideParams::from_json("{\"understanding\": 0}");
  auto on = GuideParams::from_json("{\"understanding\": 1}");
  ASSERT_TRUE(off && on);
  EXPECT_EQ(off->hash(), def.hash());
  EXPECT_NE(on->hash(), def.hash());
  EXPECT_DOUBLE_EQ(on->understanding, 1.0);

  // Engine with the flag off keeps platform kinds untouched (a table top labelled "floor" stays "floor").
  GuideEngine e0(def);
  e0.on_plane(plane("t", "floor", {1, 0.75, 1}, {0, 1, 0}, {1, 0, 0}, 1, 1));
  e0.on_plane(plane("f", "floor", {2, 0.0, 2}, {0, 1, 0}, {1, 0, 0}, 4, 4));
  EXPECT_EQ(e0.map().planes().at("t").kind, "floor");
  EXPECT_FALSE(e0.map().classify_enabled());
  GuideEngine e1(*on);
  EXPECT_TRUE(e1.map().classify_enabled());

  // Sim reports with the flag off carry no table-top planes: identical for a default config and an explicit 0.
  sim::Config c;
  c.room = "furniture", c.seed = 3, c.max_s = 40;
  std::string a = sim::simulate(c);
  c.guide.understanding = 0;
  EXPECT_EQ(sim::simulate(c), a);
}

TEST(Understanding, SimWithUnderstandingIsDeterministicAndLabelsTops) {
  sim::Config c;
  c.room = "furniture", c.seed = 5, c.max_s = 60;
  c.guide.understanding = 1;
  std::string a = sim::simulate(c), b = sim::simulate(c);
  EXPECT_EQ(a, b);
  EXPECT_FALSE(a.empty());
}

// ======================= step 2: SparsePointStore + OpeningDetector =======================
namespace {

std::vector<Vec3> store_dump(const SparsePointStore& s) {
  std::vector<Vec3> v;
  s.for_each([&](const SparsePointStore::Voxel& x) { v.push_back(x.mean()); });
  return v;
}

/// Wall z = 0, x 0..5, y 0..2.5, facing +z (into the room).
Plane test_wall() {
  Plane w;
  w.id = "w", w.kind = "wall", w.src_kind = "wall";
  w.c = {2.5, 1.25, 0}, w.n = {0, 0, 1}, w.u = {1, 0, 0}, w.v = w.n.cross(w.u);
  w.eu = 5, w.ev = 2.5;
  return w;
}

using Rect = std::array<double, 4>;  // {x0, x1, y0, y1} on the test wall

void fill_wall(SparsePointStore& s, double step, double drop, uint64_t seed, const std::vector<Rect>& holes, bool through) {
  Rng rng(seed);
  for (double y = step / 2; y < 2.5; y += step)
    for (double x = step / 2; x < 5.0; x += step) {
      bool hole = false;
      for (auto& h : holes) hole |= x > h[0] && x < h[1] && y > h[2] && y < h[3];
      const double r = rng.uniform();
      if (hole) {
        if (through && r < 0.3) s.add({x, y, -0.6});  // the corridor behind the opening
        continue;
      }
      if (r < drop) continue;
      s.add({x + 0.01 * rng.normal(), y + 0.01 * rng.normal(), 0.01 * rng.normal()});
    }
}

bool near_opening(const Opening& o, OpeningKind k, Rect t, double z, double tol) {
  double a = std::min(o.x0, o.x1), b = std::max(o.x0, o.x1);
  return o.kind == k && std::fabs(a - t[0]) <= tol && std::fabs(b - t[1]) <= tol && std::fabs(o.z0 - z) <= tol &&
         std::fabs(o.z1 - z) <= tol && std::fabs(o.y0 - t[2]) <= tol && std::fabs(o.y1 - t[3]) <= tol;
}

struct SimOpening {
  std::string kind;
  double x0, z0, x1, z1, y0, y1, conf;
};

/// Reads the "openings" array of a sim report (flat objects only).
std::vector<SimOpening> sim_openings(const std::string& report, bool* present = nullptr) {
  std::vector<SimOpening> out;
  auto at = report.find("\"openings\":[");
  if (present) *present = at != std::string::npos;
  if (at == std::string::npos) return out;
  auto end = report.find(']', at);
  size_t p = at;
  while ((p = report.find('{', p)) != std::string::npos && p < end) {
    size_t q = report.find('}', p);
    auto obj = json::parse_flat(std::string_view(report).substr(p, q - p + 1));
    EXPECT_TRUE(obj.has_value());
    if (obj) {
      auto d = [&](const char* k) { return std::get<double>(obj->at(k)); };
      out.push_back({std::get<std::string>(obj->at("kind")), d("x0"), d("z0"), d("x1"), d("z1"), d("y0"), d("y1"), d("conf")});
    }
    p = q;
  }
  return out;
}

/// Matches a reported opening against a room's OpeningSpec (world endpoints of the wall segment).
bool sim_match(const SimOpening& o, const sim::Room& room, const sim::OpeningSpec& sp, double tol) {
  auto [x0, z0] = room.poly[size_t(sp.wall)];
  auto [x1, z1] = room.poly[(size_t(sp.wall) + 1) % room.poly.size()];
  double L = std::hypot(x1 - x0, z1 - z0), ux = (x1 - x0) / L, uz = (z1 - z0) / L;
  double ax = x0 + ux * sp.u0, az = z0 + uz * sp.u0, bx = x0 + ux * sp.u1, bz = z0 + uz * sp.u1;
  auto ok = [&](double px, double pz, double qx, double qz) {
    return std::hypot(px - ax, pz - az) <= tol && std::hypot(qx - bx, qz - bz) <= tol;
  };
  const char* kind = sp.kind == 0 ? "door" : "window";
  return o.kind == kind && (ok(o.x0, o.z0, o.x1, o.z1) || ok(o.x1, o.z1, o.x0, o.z0)) && std::fabs(o.y0 - sp.y0) <= tol &&
         std::fabs(o.y1 - sp.y1) <= tol;
}

}  // namespace

// ---- SparsePointStore ----
TEST(Understanding, PointStoreVoxelsAndMeans) {
  SparsePointStore s(0.05, 100);
  EXPECT_TRUE(s.add({0.01, 0.01, 0.01}));
  EXPECT_TRUE(s.add({0.03, 0.03, 0.03}));  // same 5 cm voxel
  EXPECT_TRUE(s.add({0.07, 0.01, 0.01}));  // next voxel along x
  EXPECT_FALSE(s.add({std::nan(""), 0, 0}));
  ASSERT_EQ(s.size(), 2u);
  auto v = store_dump(s);
  EXPECT_NEAR(v[0].x(), 0.02, 1e-12);
  EXPECT_NEAR(v[1].x(), 0.07, 1e-12);
  EXPECT_EQ(s.version(), 3u);
  s.add({-0.01, 0.01, 0.01});  // negative coordinates do not alias with positive ones
  EXPECT_EQ(s.size(), 3u);
}

TEST(Understanding, PointStoreBoundedLruEviction) {
  SparsePointStore s(0.05, 3);
  s.add({0.0, 0, 0}), s.add({1.0, 0, 0}), s.add({2.0, 0, 0});
  s.add({0.01, 0, 0});  // touches voxel 0: now voxel 1 is the oldest
  s.add({3.0, 0, 0});   // evicts voxel 1
  EXPECT_EQ(s.size(), 3u);
  EXPECT_EQ(s.evicted(), 1u);
  std::vector<double> xs;
  for (auto& p : store_dump(s)) xs.push_back(std::round(p.x()));
  EXPECT_EQ(xs, (std::vector<double>{0, 2, 3}));
  // Deterministic: the same input sequence gives the same content, whatever the hash layout.
  SparsePointStore a(0.05, 500), b(0.05, 500);
  Rng r1(7), r2(7);
  for (int i = 0; i < 5000; ++i) {
    a.add({r1.uniform(0, 3), r1.uniform(0, 3), r1.uniform(0, 3)});
    b.add({r2.uniform(0, 3), r2.uniform(0, 3), r2.uniform(0, 3)});
  }
  EXPECT_EQ(a.size(), 500u);
  auto da = store_dump(a), db = store_dump(b);
  ASSERT_EQ(da.size(), db.size());
  for (size_t i = 0; i < da.size(); ++i) EXPECT_EQ(da[i], db[i]);
}

// ---- OpeningDetector on a synthetic wall ----
TEST(Understanding, OpeningDetectorDoorAndWindow) {
  const Rect door{1.0, 1.9, 0.0, 2.0}, win{3.0, 4.2, 0.9, 1.9};
  const Vec3 room(2.5, 1.4, 2.0);
  OpeningDetector det;
  struct Row {
    const char* what;
    double step, drop;
    bool through;
  };
  for (const Row& r : {Row{"dense 5 cm", 0.05, 0.0, false}, Row{"10 cm, 15% dropped", 0.10, 0.15, false},
                       Row{"10 cm + through-points", 0.10, 0.0, true}})
    for (uint64_t seed : {1u, 2u, 3u}) {
      SparsePointStore s;
      fill_wall(s, r.step, r.drop, seed, {door, win}, r.through);
      auto ops = det.detect_wall(test_wall(), true, 0.0, room, s);
      ASSERT_EQ(ops.size(), 2u) << r.what << " seed " << seed;
      int nd = 0, nw = 0;
      for (auto& o : ops) {
        nd += near_opening(o, OpeningKind::Door, door, 0.0, 0.15);
        nw += near_opening(o, OpeningKind::Window, win, 0.0, 0.15);
        EXPECT_GE(o.conf, det.params().min_conf);
        EXPECT_LE(o.conf, 1.0);
        EXPECT_TRUE(o.wall.empty());  // the shell attaches wall ids (step 6)
      }
      EXPECT_EQ(nd, 1) << r.what << " seed " << seed;
      EXPECT_EQ(nw, 1) << r.what << " seed " << seed;
    }
  // Through-points raise the confidence of the same opening.
  SparsePointStore plain, thr;
  fill_wall(plain, 0.1, 0.0, 1, {door}, false);
  fill_wall(thr, 0.1, 0.0, 1, {door}, true);
  auto a = det.detect_wall(test_wall(), true, 0.0, room, plain), b = det.detect_wall(test_wall(), true, 0.0, room, thr);
  ASSERT_EQ(a.size(), 1u);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_GT(b[0].conf, a[0].conf);
}

TEST(Understanding, OpeningDetectorRejects) {
  const Vec3 room(2.5, 1.4, 2.0);
  OpeningDetector det;
  auto run = [&](std::vector<Rect> holes, bool has_floor = true) {
    SparsePointStore s;
    fill_wall(s, 0.1, 0.1, 4, holes, false);
    return det.detect_wall(test_wall(), has_floor, 0.0, room, s);
  };
  EXPECT_TRUE(run({}).empty());                              // plain wall
  EXPECT_TRUE(run({{1.0, 3.0, 0.0, 2.0}}).empty());          // floor-touching but 2 m wide (wardrobe shadow, open plan)
  EXPECT_TRUE(run({{1.0, 1.9, 0.0, 1.2}}).empty());          // floor-touching but low (furniture shadow)
  EXPECT_TRUE(run({{1.0, 1.9, 1.5, 2.6}}).empty());          // reaches the top of the observed wall: unknown
  EXPECT_TRUE(run({{0.0, 0.9, 0.0, 2.0}}).empty());          // at the edge of the observed wall
  EXPECT_TRUE(run({{3.0, 4.2, 0.2, 1.0}}).empty());          // sill 0.2 m: not a window
  EXPECT_TRUE(run({{1.0, 1.9, 0.0, 2.0}}, false).empty());   // no floor known: no door
  EXPECT_TRUE(run({{2.0, 2.2, 1.0, 1.2}}).empty());          // tiny hole
  LiveMap m;  // empty store, no cameras: nothing
  EXPECT_TRUE(OpeningDetector().detect(m, SparsePointStore()).empty());
}

TEST(Understanding, OpeningDetectorIsOrientationFree) {
  const Rect door{1.0, 1.9, 0.0, 2.0};
  SparsePointStore s;
  fill_wall(s, 0.1, 0.1, 9, {door}, true);
  OpeningDetector det;
  Plane w = test_wall();
  auto a = det.detect_wall(w, true, 0.0, {2.5, 1.4, 2.0}, s);
  // Same wall with the platform normal pointing out of the room and flipped in-plane axes.
  w.n = -w.n, w.u = -w.u, w.v = w.n.cross(w.u);
  auto b = det.detect_wall(w, true, 0.0, {2.5, 1.4, 2.0}, s);
  ASSERT_EQ(a.size(), 1u);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_TRUE(near_opening(b[0], OpeningKind::Door, door, 0.0, 0.15));
  EXPECT_NEAR(a[0].conf, b[0].conf, 1e-9);
}

TEST(Understanding, OpeningDetectorNeedsViewsForFreeSpace) {
  // A gap with fewer than min_cell_views views is unknown, never opening evidence.
  const Rect door{1.0, 1.9, 0.0, 2.0}, win{3.0, 4.2, 0.9, 1.9};
  const Vec3 room(2.5, 1.4, 2.0);
  SparsePointStore s;
  fill_wall(s, 0.1, 0.1, 5, {door, win}, false);
  OpeningDetector det;
  const int k = det.params().min_cell_views;
  ASSERT_GE(k, 2);
  auto all = [](int v) { return [v](const Vec3&) { return v; }; };
  EXPECT_TRUE(det.detect_wall(test_wall(), true, 0.0, room, s, all(k - 1)).empty());
  EXPECT_TRUE(det.detect_wall(test_wall(), true, 0.0, room, s, all(0)).empty());
  EXPECT_EQ(det.detect_wall(test_wall(), true, 0.0, room, s, all(k)).size(), 2u);
  // Door gap under-viewed, the rest of the wall well viewed: only the window remains.
  auto ops = det.detect_wall(test_wall(), true, 0.0, room, s, [&](const Vec3& p) {
    return p.x() > door[0] && p.x() < door[1] && p.y() < door[3] ? k - 1 : k;
  });
  ASSERT_EQ(ops.size(), 1u);
  EXPECT_TRUE(near_opening(ops[0], OpeningKind::Window, win, 0.0, 0.15));
  // Coverage::views_at: no grid for the plane -> 0 views.
  EXPECT_EQ(Coverage(GuideParams{}).views_at("nope", Vec3(0, 0, 0)), 0);
}

// ---- sim: the door_window scene recovers both openings; plain walls stay clean ----
namespace {
/// Understanding-on sims are slow (~0.4 s per simulated second): run independent seeds in parallel. simulate() is
/// pure (no shared state), so this changes nothing but wall time. The walker holds the phone level between tilts
/// (Human::hold_level, ADR 0015), points come from what each frame views (Config::view_points) and it walks
/// kUnderstandingSimS seconds.
constexpr double kUnderstandingSimS = 150;
std::vector<std::string> run_sims(const std::vector<std::pair<std::string, uint64_t>>& runs, double understanding) {
  std::vector<std::string> out(runs.size());
  std::vector<std::thread> th;
  for (size_t k = 0; k < runs.size(); ++k)
    th.emplace_back([&, k] {
      sim::Config c;
      c.room = runs[k].first, c.seed = runs[k].second;
      c.guide.understanding = understanding;
      c.human.hold_level = true;
      c.view_points = true;
      c.max_s = kUnderstandingSimS;
      out[k] = sim::simulate(c);
    });
  for (auto& t : th) t.join();
  return out;
}
}  // namespace

TEST(Understanding, SimRecoversDoorAndWindow) {
  const auto room = sim::make_room("door_window");
  ASSERT_EQ(room.openings.size(), 2u);
  std::cout << "\n  Understanding.SimRecoversDoorAndWindow (synthetic points, " << kUnderstandingSimS
            << " s level walks, tolerance 0.25 m)\n";
  auto reps = run_sims({{"door_window", 1}, {"door_window", 2}, {"door_window", 3}}, 1);
  int total = 0;
  for (uint64_t seed : {1u, 2u, 3u}) {
    const std::string& rep = reps[seed - 1];
    auto ops = sim_openings(rep);
    int found = 0;
    std::vector<bool> used(ops.size(), false);
    for (auto& sp : room.openings) {
      int m = 0;
      for (size_t k = 0; k < ops.size(); ++k)
        if (sim_match(ops[k], room, sp, 0.25)) ++m, used[k] = true;
      EXPECT_LE(m, 1) << "seed " << seed << " opening kind " << sp.kind << " reported twice";
      found += m >= 1;
    }
    const int false_ops = int(std::count(used.begin(), used.end(), false));
    EXPECT_GE(found, 1) << "seed " << seed;
    EXPECT_EQ(false_ops, 0) << "seed " << seed << " (false openings)";
    total += found;
    auto ps = rep.find("\"points_stored\":");
    std::cout << "  seed " << seed << ": " << found << "/2 recovered, " << ops.size() << " reported, "
              << rep.substr(ps, rep.find_first_of(",}", ps) - ps) << "\n";
    for (auto& o : ops)
      std::cout << "    " << o.kind << " (" << o.x0 << "," << o.z0 << ")-(" << o.x1 << "," << o.z1 << ") y " << o.y0 << ".."
                << o.y1 << " conf " << o.conf << "\n";
  }
  std::cout << "  total: " << total << "/6 recovered (pass >= 5)\n";
  EXPECT_GE(total, 5);
}

namespace {
/// Plain walls (no openings): the detector must report none.
void expect_no_false_openings(const std::vector<std::pair<std::string, uint64_t>>& runs) {
  auto reps = run_sims(runs, 1);
  for (size_t k = 0; k < runs.size(); ++k) {
    bool present = false;
    auto ops = sim_openings(reps[k], &present);
    EXPECT_TRUE(present) << runs[k].first;
    EXPECT_TRUE(ops.empty()) << runs[k].first << " seed " << runs[k].second << ": " << ops.size() << " false openings";
    for (auto& o : ops)
      std::cout << "    false " << o.kind << " (" << o.x0 << "," << o.z0 << ")-(" << o.x1 << "," << o.z1 << ") y " << o.y0
                << ".." << o.y1 << " conf " << o.conf << "\n";
  }
}
}  // namespace

TEST(Understanding, SimNoFalseOpeningsOnBoxWalls) { expect_no_false_openings({{"box", 1}, {"box", 2}, {"box", 3}}); }

TEST(Understanding, SimNoFalseOpeningsOnLWalls) { expect_no_false_openings({{"L", 1}}); }

TEST(Understanding, SimFlagOffReportHasNoOpenings) {
  // Flag off: the report has no openings key at all (Phase 8 reports unchanged).
  sim::Config off;
  off.room = "door_window", off.seed = 1, off.max_s = 30;
  bool present = true;
  sim_openings(sim::simulate(off), &present);
  EXPECT_FALSE(present);
}

// ---- step 3: NavGrid object boxes, ObjectLifter, detector oracle ----
namespace {

Object make_obj(ObjectClass c, Vec3 centre, Vec3 size, double yaw) {
  Object o;
  o.cls = c, o.centre = centre, o.size = size, o.yaw = yaw;
  return o;
}

/// Corners of an object's box in the world frame (yaw about +Y from +Z, depth axis (sin, cos)).
std::vector<Vec3> box_corners(const Object& o) {
  std::vector<Vec3> out;
  const double ca = std::cos(o.yaw), sa = std::sin(o.yaw);
  for (int i = 0; i < 8; ++i) {
    const double lx = (i & 1 ? 0.5 : -0.5) * o.size.x(), ly = (i & 2 ? 0.5 : -0.5) * o.size.y(),
                 lz = (i & 4 ? 0.5 : -0.5) * o.size.z();
    out.push_back({o.centre.x() + lx * ca + lz * sa, o.centre.y() + ly, o.centre.z() - lx * sa + lz * ca});
  }
  return out;
}

/// Points on the five faces (top + 4 sides) of an object at `step` spacing.
void fill_box(SparsePointStore& s, const Object& o, double step) {
  const double ca = std::cos(o.yaw), sa = std::sin(o.yaw);
  auto add = [&](double lx, double ly, double lz) {
    s.add({o.centre.x() + lx * ca + lz * sa, o.centre.y() + ly, o.centre.z() - lx * sa + lz * ca});
  };
  const double hx = 0.5 * o.size.x(), hy = 0.5 * o.size.y(), hz = 0.5 * o.size.z();
  for (double a = -hx; a <= hx; a += step)
    for (double b = -hz; b <= hz; b += step) add(a, hy, b);
  for (double a = -hy; a <= hy; a += step) {
    for (double b = -hx; b <= hx; b += step) add(b, a, -hz), add(b, a, hz);
    for (double b = -hz; b <= hz; b += step) add(-hx, a, b), add(hx, a, b);
  }
}

Detection project_box(const Object& o, const LiftContext& ctx, double score = 0.8) {
  Detection d;
  d.cls = o.cls, d.score = float(score), d.x0 = d.y0 = 1e9f, d.x1 = d.y1 = -1e9f;
  for (auto& c : box_corners(o)) {
    auto px = geometry::project(ctx.K, ctx.T_world_from_camera, c);
    if (!px) continue;
    d.x0 = std::min(d.x0, float((*px)[0])), d.x1 = std::max(d.x1, float((*px)[0]));
    d.y0 = std::min(d.y0, float((*px)[1])), d.y1 = std::max(d.y1, float((*px)[1]));
  }
  return d;
}

LiftContext lift_ctx(Vec3 cam, double yaw, double pitch) {
  LiftContext c;
  c.T_world_from_camera = pose_from_yaw_pitch(cam, yaw, pitch);
  c.K.width = 640, c.K.height = 480, c.K.fx = c.K.fy = 500, c.K.cx = 319.5, c.K.cy = 239.5;
  c.has_floor = true, c.floor_y = 0;
  PlaneGeom wall, floor;
  wall.c = {0, 1.25, 3.5}, wall.n = {0, 0, -1};
  floor.c = {0, 0, 0}, floor.n = {0, 1, 0};
  c.surfaces = {wall, floor};
  return c;
}

}  // namespace

TEST(Understanding, NavGridObjectBox) {
  GuideParams gp;
  NavGrid g(gp);
  g.set_floor_y(0);
  // Obstacle regardless of points: an axis-aligned 1.0 x 0.6 m couch (0.8 m high) at (2, 3).
  Object couch = make_obj(ObjectClass::Couch, {2.0, 0.4, 3.0}, {1.0, 0.8, 0.6}, 0.0);
  ASSERT_TRUE(g.add_object_box(couch));
  int ix, iz;
  g.to_cell({2.0, 0, 3.0}, ix, iz);
  EXPECT_TRUE(g.obstacle(ix, iz));
  g.to_cell({1.55, 0, 3.0}, ix, iz);
  EXPECT_TRUE(g.obstacle(ix, iz));  // inside the width
  g.to_cell({1.3, 0, 3.0}, ix, iz);
  EXPECT_FALSE(g.obstacle(ix, iz));  // 0.7 m left of the centre: outside 0.5 m + half a cell
  g.to_cell({2.0, 0, 3.45}, ix, iz);
  EXPECT_FALSE(g.obstacle(ix, iz));  // outside the depth (0.3 + 0.05)
  EXPECT_GT(g.obstacle_cells(), 40u);
  EXPECT_LT(g.obstacle_cells(), 90u);
  // Rotated 90 deg: the long side now runs along z.
  NavGrid r(gp);
  r.set_floor_y(0);
  Object turned = couch;
  turned.yaw = kPi / 2;
  ASSERT_TRUE(r.add_object_box(turned));
  r.to_cell({2.0, 0, 3.45}, ix, iz);
  EXPECT_TRUE(r.obstacle(ix, iz));
  r.to_cell({2.45, 0, 3.0}, ix, iz);
  EXPECT_FALSE(r.obstacle(ix, iz));
  // Wall-mounted classes and objects below obstacle_min_h (0.2 m) are not obstacles.
  NavGrid n(gp);
  n.set_floor_y(0);
  EXPECT_FALSE(n.add_object_box(make_obj(ObjectClass::Tv, {2, 1.2, 3}, {1.0, 0.6, 0.1}, 0)));
  EXPECT_FALSE(n.add_object_box(make_obj(ObjectClass::Laptop, {2, 0.015, 3}, {0.35, 0.03, 0.25}, 0)));
  EXPECT_EQ(n.obstacle_cells(), 0u);
}

TEST(Understanding, LifterFromPointsRecoversBox) {
  // A 1.2 x 0.8 x 0.9 m table-like block turned 30 deg, seen from 2.5 m with a wall behind it.
  const Object truth = make_obj(ObjectClass::DiningTable, {2.5, 0.45, 1.5}, {1.2, 0.9, 0.8}, deg2rad(30));
  LiftContext ctx = lift_ctx({2.5, 1.4, -1.0}, 0.0, deg2rad(-15));
  SparsePointStore s;
  fill_box(s, truth, 0.05);
  for (double x = 0; x < 5; x += 0.05)  // wall points behind and floor points around: not object points
    for (double y = 0; y < 2.5; y += 0.1) s.add({x, y, 3.5});
  for (double x = 1; x < 4; x += 0.1)
    for (double z = 0; z < 3; z += 0.1) s.add({x, 0.01, z});
  ObjectLifter lifter;
  Detection d = project_box(truth, ctx);
  auto L = lifter.lift(d, ctx, s);
  ASSERT_TRUE(L.has_value());
  EXPECT_TRUE(L->from_points);
  EXPECT_LT((L->centre - truth.centre).norm(), 0.15);
  EXPECT_NEAR(L->size.y(), 0.9, 0.1);
  // Rectangle sizes are symmetric in x/z up to 90 deg: compare the sorted footprint.
  EXPECT_NEAR(std::max(L->size.x(), L->size.z()), 1.2, 0.2);
  EXPECT_NEAR(std::min(L->size.x(), L->size.z()), 0.8, 0.2);
  // A box over bare wall points gives no point-based object.
  Detection wall_only = d;
  wall_only.x0 = 0, wall_only.x1 = 100, wall_only.y0 = 0, wall_only.y1 = 100;
  SparsePointStore bg;
  for (double x = 0; x < 5; x += 0.05)
    for (double y = 0; y < 2.5; y += 0.1) bg.add({x, y, 3.5});
  auto F = lifter.lift(wall_only, ctx, bg);
  EXPECT_TRUE(!F || !F->from_points);
}

TEST(Understanding, LifterFloorRayFallback) {
  // No points: the bottom-centre ray meets the floor and the class prior gives the size.
  const Object truth = make_obj(ObjectClass::Chair, {2.5, 0.45, 3.0}, {0.5, 0.9, 0.5}, 0.0);
  LiftContext ctx = lift_ctx({2.5, 1.4, 0.0}, 0.0, deg2rad(-10));
  ObjectLifter lifter;
  auto L = lifter.lift(project_box(truth, ctx), ctx, SparsePointStore());
  ASSERT_TRUE(L.has_value());
  EXPECT_FALSE(L->from_points);
  EXPECT_LT(std::hypot(L->centre.x() - truth.centre.x(), L->centre.z() - truth.centre.z()), 0.3);
  EXPECT_NEAR(L->size.y(), 0.9, 0.05);
  // Bottom cut off by the image edge, or no floor reference: no lift.
  Detection cut = project_box(truth, ctx);
  cut.y1 = 480;
  EXPECT_FALSE(lifter.lift(cut, ctx, SparsePointStore()).has_value());
  LiftContext nofloor = ctx;
  nofloor.has_floor = false;
  EXPECT_FALSE(lifter.lift(project_box(truth, ctx), nofloor, SparsePointStore()).has_value());
}

TEST(Understanding, LifterFloorRayFallbackUsesTheNewClassPriors) {
  // A refrigerator (class 11, appended in ADR 0030) lifts through the same fallback with its own prior height.
  const Object truth = make_obj(ObjectClass::Refrigerator, {2.5, 0.875, 3.5}, {0.7, 1.75, 0.7}, 0.0);
  LiftContext ctx = lift_ctx({2.5, 1.4, 0.0}, 0.0, deg2rad(-10));
  ObjectLifter lifter;
  Detection d = project_box(truth, ctx);
  ASSERT_LT(d.y1, 477) << "fixture: the bottom edge must be inside the image";
  auto L = lifter.lift(d, ctx, SparsePointStore());
  ASSERT_TRUE(L.has_value());
  EXPECT_FALSE(L->from_points);
  EXPECT_NEAR(L->size.y(), 1.75, 0.05);
  EXPECT_LT(std::hypot(L->centre.x() - truth.centre.x(), L->centre.z() - truth.centre.z()), 0.4);
}

TEST(Understanding, LifterTracksAndPublishes) {
  const Object truth = make_obj(ObjectClass::Chair, {2.5, 0.45, 3.0}, {0.5, 0.9, 0.5}, 0.0);
  LiftContext ctx = lift_ctx({2.5, 1.4, 0.0}, 0.0, deg2rad(-10));
  const SparsePointStore none;
  ObjectLifter lifter;
  EXPECT_TRUE(lifter.objects().empty());  // no detections: nothing
  Detection d = project_box(truth, ctx);
  d.frame_id = 1;
  ASSERT_TRUE(lifter.on_detection(d, ctx, none));
  EXPECT_TRUE(lifter.objects().empty());  // one observation is not enough
  EXPECT_FALSE(lifter.on_detection(d, ctx, none));  // the same frame never counts twice
  d.frame_id = 2;
  ASSERT_TRUE(lifter.on_detection(d, ctx, none));
  EXPECT_TRUE(lifter.objects().empty());  // floor-ray-only tracks need fallback_obs (3)
  d.frame_id = 3;
  ASSERT_TRUE(lifter.on_detection(d, ctx, none));
  ASSERT_EQ(lifter.objects().size(), 1u);
  EXPECT_EQ(lifter.objects()[0].n_obs, 3);
  EXPECT_EQ(lifter.objects()[0].cls, ObjectClass::Chair);
  EXPECT_GT(lifter.objects()[0].conf, 0.0);
  EXPECT_LE(lifter.objects()[0].conf, 1.0);
  // Same place, other class: a second track. A lone false detection stays unpublished. Low score and unknown are ignored.
  Detection other = d;
  other.cls = ObjectClass::PottedPlant;
  other.frame_id = 4;
  EXPECT_TRUE(lifter.on_detection(other, ctx, none));
  EXPECT_EQ(lifter.tracks(), 2u);
  EXPECT_EQ(lifter.objects().size(), 1u);
  Detection weak = d;
  weak.score = 0.1f;
  EXPECT_FALSE(lifter.on_detection(weak, ctx, none));
  Detection unk = d;
  unk.cls = ObjectClass::Unknown;
  EXPECT_FALSE(lifter.on_detection(unk, ctx, none));
  EXPECT_EQ(lifter.tracks(), 2u);
  // Deterministic: same inputs, same objects.
  ObjectLifter again;
  for (uint32_t f = 1; f <= 3; ++f) d.frame_id = f, again.on_detection(d, ctx, none);
  EXPECT_EQ(again.objects()[0].centre, lifter.objects()[0].centre);
}

TEST(Understanding, LifterFallbackSwapsAxesInOddQuadrant) {
  // View bearing 120 deg (second quadrant): the yaw reduced to [0, 90) is 30 deg, so the 1.9 x 0.9 couch's width and
  // depth swap. Without the swap the 1.9 m side would point along the view direction instead of across it.
  const double bearing = deg2rad(120);
  const Vec3 cam(4.0, 1.4, 3.0), hv(std::sin(bearing), 0, std::cos(bearing));
  LiftContext ctx = lift_ctx(cam, bearing, deg2rad(-10));
  const Vec3 c = cam + hv * 3.2;
  const Object truth = make_obj(ObjectClass::Couch, {c.x(), 0.425, c.z()}, {1.9, 0.85, 0.9}, bearing);
  ObjectLifter lifter;
  auto L = lifter.lift(project_box(truth, ctx), ctx, SparsePointStore());
  ASSERT_TRUE(L.has_value());
  EXPECT_FALSE(L->from_points);
  EXPECT_NEAR(rad2deg(L->yaw), 30.0, 0.5);
  EXPECT_NEAR(L->size.x(), 0.9, 1e-9);  // depth (0.9) lies along the view direction = this box's width axis
  EXPECT_NEAR(L->size.z(), 1.9, 1e-9);
  // Footprint matches the truth box: the nearest and farthest corners of the lifted box sit where the truth's do.
  Object lifted = make_obj(ObjectClass::Couch, L->centre, L->size, L->yaw);
  for (const Vec3& p : box_corners(lifted)) {
    double best = 1e9;
    for (const Vec3& q : box_corners(truth)) best = std::min(best, (p - q).norm());
    EXPECT_LT(best, 0.35);  // the floor ray reads the front edge; the rest is the class prior
  }
}

TEST(Understanding, LifterEvictsOnlyUnpublishedTracks) {
  LifterParams lp;
  lp.max_tracks = 2;
  LiftContext ctx = lift_ctx({2.5, 1.4, 0.0}, 0.0, deg2rad(-10));
  const SparsePointStore none;
  const Object chair = make_obj(ObjectClass::Chair, {2.5, 0.45, 3.0}, {0.5, 0.9, 0.5}, 0.0);
  const Object bed = make_obj(ObjectClass::Bed, {2.5, 0.275, 3.0}, {1.9, 0.55, 1.4}, 0.0);
  auto see = [&](ObjectLifter& l, const Object& o, ObjectClass cls, uint32_t from, int n) {
    Detection d = project_box(o, ctx);
    d.cls = cls;
    for (int i = 0; i < n; ++i) d.frame_id = from + uint32_t(i), l.on_detection(d, ctx, none);
  };
  {  // one visible (3 floor-ray observations), one not (1): the unseen one makes room
    ObjectLifter l(lp);
    see(l, chair, ObjectClass::Chair, 1, 3);
    see(l, chair, ObjectClass::PottedPlant, 10, 1);
    ASSERT_EQ(l.tracks(), 2u);
    EXPECT_TRUE(l.on_detection([&] { Detection d = project_box(bed, ctx); d.frame_id = 20; return d; }(), ctx, none));
    EXPECT_EQ(l.tracks(), 2u);
    ASSERT_EQ(l.objects().size(), 1u);
    EXPECT_EQ(l.objects()[0].cls, ObjectClass::Chair);
  }
  {  // both reported (2 observations are not enough for a floor-ray track, so use 3): nothing is evicted
    ObjectLifter l(lp);
    see(l, chair, ObjectClass::Chair, 1, 3);
    see(l, chair, ObjectClass::PottedPlant, 10, 3);
    ASSERT_EQ(l.objects().size(), 2u);
    Detection d = project_box(bed, ctx);
    d.frame_id = 20;
    EXPECT_FALSE(l.on_detection(d, ctx, none));
    EXPECT_EQ(l.tracks(), 2u);
    EXPECT_EQ(l.objects().size(), 2u);
  }
}

namespace {
struct OracleObj {
  std::string cls;
  double x, y, z, w, h, d, conf;
  int n_obs;
};
/// Reads one flat-object array of the sim report's "oracle" block ("objects" or "truth").
std::vector<OracleObj> oracle_array(const std::string& report, const char* key) {
  std::vector<OracleObj> out;
  auto o = report.find("\"oracle\":{");
  EXPECT_NE(o, std::string::npos);
  if (o == std::string::npos) return out;
  auto at = report.find(std::string("\"") + key + "\":[", o);
  if (at == std::string::npos) return out;
  auto end = report.find(']', at);
  size_t p = at;
  while ((p = report.find('{', p)) != std::string::npos && p < end) {
    size_t q = report.find('}', p);
    auto obj = json::parse_flat(std::string_view(report).substr(p, q - p + 1));
    EXPECT_TRUE(obj.has_value());
    if (obj) {
      auto d = [&](const char* k) { return obj->count(k) ? std::get<double>(obj->at(k)) : 0.0; };
      out.push_back({std::get<std::string>(obj->at("cls")), d("x"), d("y"), d("z"), d("w"), d("h"), d("d"), d("conf"), int(d("n_obs"))});
    }
    p = q;
  }
  return out;
}
double oracle_num(const std::string& report, const char* key) {
  auto o = report.find("\"oracle\":{");
  auto at = report.find(std::string("\"") + key + "\":", o);
  return std::atof(report.c_str() + at + std::strlen(key) + 3);
}
/// A sim report with the "oracle" block cut out (it is the only thing the oracle adds).
std::string without_oracle(std::string r) {
  auto a = r.find(",\"oracle\":{");
  if (a == std::string::npos) a = r.find("\"oracle\":{");
  if (a == std::string::npos) return r;
  size_t b = r.find('{', a), depth = 0;
  for (; b < r.size(); ++b) {
    depth += r[b] == '{', depth -= r[b] == '}';
    if (depth == 0) break;
  }
  r.erase(a, b + 1 - a);
  return r;
}
}  // namespace

TEST(Understanding, SimOracleLiftsFurniture) {
  const auto room = sim::make_room("objects");
  ASSERT_EQ(room.boxes.size(), 4u);
  std::vector<std::string> reps(3);
  std::vector<std::thread> th;
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t k = 0; k < 3; ++k)
    th.emplace_back([&, k] {
      sim::Config c;
      c.room = "objects", c.seed = k + 1, c.max_s = 150;
      c.policy = "scripted", c.view_points = true;  // perimeter loops + centre spins: the lifter is tested in isolation
      c.detector_oracle = true;
      reps[k] = sim::simulate(c);
    });
  for (auto& t : th) t.join();
  const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "\n  Understanding.SimOracleLiftsFurniture (miss 15%, false 8%/keyframe, centre <= 0.3 m, height <= 0.25 m), wall "
            << wall_s << " s\n";
  int found = 0, total = 0;
  for (size_t k = 0; k < 3; ++k) {
    auto lifted = oracle_array(reps[k], "objects"), truth = oracle_array(reps[k], "truth");
    ASSERT_EQ(truth.size(), 4u);
    std::vector<bool> used(lifted.size(), false);
    for (auto& t : truth) {
      ++total;
      double best = 1e9, bh = 0;
      size_t bi = lifted.size();
      for (size_t i = 0; i < lifted.size(); ++i) {
        if (used[i] || lifted[i].cls != t.cls) continue;
        double e = std::sqrt(std::pow(lifted[i].x - t.x, 2) + std::pow(lifted[i].y - t.y, 2) + std::pow(lifted[i].z - t.z, 2));
        if (e < best) best = e, bh = std::fabs(lifted[i].h - t.h), bi = i;
      }
      const bool ok = bi < lifted.size() && best <= 0.3 && bh <= 0.25;
      if (ok) used[bi] = true, ++found;
      std::cout << "  seed " << k + 1 << " " << t.cls << ": "
                << (bi < lifted.size() ? "centre err " + std::to_string(best) + " height err " + std::to_string(bh) : std::string("not lifted"))
                << (ok ? "" : "  FAIL") << "\n";
    }
    const int ghosts = int(std::count(used.begin(), used.end(), false));
    for (size_t i = 0; i < lifted.size(); ++i)
      if (!used[i]) std::cout << "  seed " << k + 1 << " ghost " << lifted[i].cls << " (" << lifted[i].x << "," << lifted[i].z << ") size " << lifted[i].w << "x" << lifted[i].d << "x" << lifted[i].h << " n_obs " << lifted[i].n_obs << "\n";
    std::cout << "  seed " << k + 1 << ": " << lifted.size() << " published, " << ghosts << " ghost(s), "
              << oracle_num(reps[k], "detections") << " detections + " << oracle_num(reps[k], "false_detections") << " false\n";
    EXPECT_EQ(ghosts, 0) << "seed " << k + 1;
  }
  std::cout << "  total: " << found << "/" << total << " lifted within tolerance\n";
  EXPECT_EQ(found, total);
  if (wall_s >= 90.0) std::cout << "  WARNING: " << wall_s << " s wall (budget 90 s)\n";
}

// NOTE (step 5): GuideEngine::on_detections exists (recorded detections only), but the sim oracle still never feeds it, so this proves the harness is inert. If the sim ever feeds the engine,
// re-assert with the engine in the loop: misses everywhere => same report.
TEST(Understanding, SimNoDetectionsLeavesGuidanceUnchanged) {
  // The oracle never feeds the engine: with every detection missed (and no false ones), the report is the flag-off report.
  for (double understanding : {0.0, 1.0}) {
    sim::Config base;
    base.room = "objects", base.seed = 2, base.max_s = 25;
    base.guide.understanding = understanding;
    sim::Config on = base;
    on.detector_oracle = true, on.oracle_miss = 1.0, on.oracle_false = 0.0;
    const std::string a = sim::simulate(base), b = sim::simulate(on);
    EXPECT_EQ(a.find("\"oracle\""), std::string::npos);
    EXPECT_EQ(oracle_num(b, "detections"), 0.0);
    EXPECT_TRUE(oracle_array(b, "objects").empty());
    EXPECT_EQ(without_oracle(b), a) << "understanding " << understanding;
    // Detections present (the oracle running for real) still do not change what the engine reports.
    sim::Config live = base;
    live.detector_oracle = true;
    EXPECT_EQ(without_oracle(sim::simulate(live)), a) << "understanding " << understanding;
  }
}

// ---- RoomShell (step 6) ----
namespace {

using Pt = std::pair<double, double>;

/// Wall plane records of a polygon, rotated by `rot` about the origin and shifted by (1, -2). `span` shrinks each
/// plane around its midpoint (a plane that has not grown yet); `skip` drops wall indices. Ids "w<i>".
std::vector<bundle::PlaneRecord> poly_walls(const std::vector<Pt>& P, double rot = 0, double span = 1.0,
                                            const std::vector<int>& skip = {}, double height = 2.5) {
  std::vector<bundle::PlaneRecord> out;
  double c = std::cos(rot), s = std::sin(rot);
  auto R = [&](Pt p) { return Pt{p.first * c - p.second * s + 1.0, p.first * s + p.second * c - 2.0}; };
  for (size_t i = 0; i < P.size(); ++i) {
    if (std::find(skip.begin(), skip.end(), int(i)) != skip.end()) continue;
    auto [x0, z0] = R(P[i]);
    auto [x1, z1] = R(P[(i + 1) % P.size()]);
    double L = std::hypot(x1 - x0, z1 - z0);
    Vec3 u((x1 - x0) / L, 0, (z1 - z0) / L), n(-(z1 - z0) / L, 0, (x1 - x0) / L);
    out.push_back(plane("w" + std::to_string(i), "wall", Vec3(0.5 * (x0 + x1), 0.5 * height, 0.5 * (z0 + z1)), n, u,
                        L * span, height));
  }
  return out;
}

const std::vector<Pt> kBox = {{0, 0}, {5, 0}, {5, 4}, {0, 4}};
const std::vector<Pt> kL = {{0, 0}, {6, 0}, {6, 3}, {3, 3}, {3, 5}, {0, 5}};

Pt xf(Pt p, double rot) {
  return {p.first * std::cos(rot) - p.second * std::sin(rot) + 1.0, p.first * std::sin(rot) + p.second * std::cos(rot) - 2.0};
}

/// Largest distance from a truth vertex to the nearest shell corner, and vice versa.
double corner_err(const ShellModel& m, const std::vector<Pt>& truth, double rot) {
  double worst = 0;
  auto nearest = [](Pt a, const std::vector<Pt>& v) {
    double d = 1e9;
    for (auto& b : v) d = std::min(d, std::hypot(a.first - b.first, a.second - b.second));
    return d;
  };
  std::vector<Pt> tr;
  for (auto& p : truth) tr.push_back(xf(p, rot));
  for (auto& p : tr) worst = std::max(worst, nearest(p, m.corners));
  for (auto& p : m.corners) worst = std::max(worst, nearest(p, tr));
  return worst;
}

RoomShell::WallIds engine_ids(const std::vector<std::string>& first_seen) {
  RoomShell::WallIds ids;
  int n = 1;
  for (auto& s : first_seen) ids[s] = n++;
  return ids;
}

bool is_horizontal(const ShellWall& w) { return std::abs(w.z0 - w.z1) < 1e-6; }

std::string line_key(const ShellWall& w) {
  char b[32];
  std::snprintf(b, sizeof b, "%c=%.0f", is_horizontal(w) ? 'z' : 'x', is_horizontal(w) ? w.z0 : w.x0);
  return b;
}

}  // namespace

TEST(Understanding, ShellCornersBoxAndL) {
  for (auto& [poly, n_walls] : {std::pair{kBox, 4}, std::pair{kL, 6}}) {
    for (double rot : {0.0, 0.5}) {  // 0.5 rad = 28.6 degrees: the shell is Manhattan in its own frame
      LiveMap m;
      std::vector<std::string> order;
      for (auto& r : poly_walls(poly, rot)) m.add_plane(r), order.push_back(r.id);
      auto ids = engine_ids(order);
      RoomShell sh;
      sh.update(m, &ids);
      const auto& s = sh.shell();
      EXPECT_EQ(int(s.walls.size()), n_walls) << "rot " << rot;
      EXPECT_TRUE(s.closed);
      EXPECT_FALSE(s.low_confidence);
      for (auto& w : s.walls) EXPECT_FALSE(w.inferred);
      EXPECT_LT(corner_err(s, poly, rot), 0.25) << "n " << n_walls << " rot " << rot;
    }
  }
}

TEST(Understanding, ShellCornersTolerateNoise) {
  LiveMap m;
  std::vector<std::string> order;
  int k = 0;
  for (auto r : poly_walls(kL, 0.0)) {  // 3 degrees of normal noise, 5 cm offsets, planes a little short
    double a = (k % 2 ? 1 : -1) * 0.05, off = 0.05 * ((k % 3) - 1);
    double ca = std::cos(a), sa = std::sin(a);
    double nx = r.normal[0] * ca - r.normal[2] * sa, nz = r.normal[0] * sa + r.normal[2] * ca;
    double ux = r.axis_u[0] * ca - r.axis_u[2] * sa, uz = r.axis_u[0] * sa + r.axis_u[2] * ca;
    r.normal[0] = nx, r.normal[2] = nz, r.axis_u[0] = ux, r.axis_u[2] = uz;
    r.center[0] += off * nx, r.center[2] += off * nz;
    r.extent_u -= 0.3;
    ++k;
    m.add_plane(r), order.push_back(r.id);
  }
  auto ids = engine_ids(order);
  RoomShell sh;
  sh.update(m, &ids);
  EXPECT_EQ(sh.shell().walls.size(), 6u);
  EXPECT_TRUE(sh.shell().closed);
  EXPECT_FALSE(sh.shell().low_confidence);
  EXPECT_LT(corner_err(sh.shell(), kL, 0.0), 0.25);
}

TEST(Understanding, ShellIdsStableUnderPlaneGrowth) {
  auto ids = engine_ids({"w0", "w1", "w2", "w3"});  // what GuideEngine::assign_wall_ids would hold (ADR 0022)
  std::map<std::string, std::string> line_id;       // wall line -> id, must never change
  RoomShell sh;                                     // one shell across the whole growth sequence
  for (double span : {0.4, 0.6, 0.8, 1.0}) {
    LiveMap m;
    for (auto& r : poly_walls(kBox, 0.0, span)) m.add_plane(r);
    for (int rep = 0; rep < 2; ++rep) {
      sh.update(m, &ids);
      const auto& s = sh.shell();
      ASSERT_EQ(s.walls.size(), 4u) << "span " << span;
      EXPECT_LT(corner_err(s, kBox, 0.0), 0.25) << "span " << span;
      for (auto& w : s.walls) {
        auto it = line_id.emplace(line_key(w), w.id).first;  // lines (shift 1,-2): z=-2 w0, x=6 w1, z=2 w2, x=1 w3
        EXPECT_EQ(it->second, w.id) << "span " << span << " line " << line_key(w);
      }
    }
  }
  EXPECT_EQ(line_id.size(), 4u);
  EXPECT_EQ(line_id["z=-2"], "W1");
  EXPECT_EQ(line_id["x=6"], "W2");
  EXPECT_EQ(line_id["z=2"], "W3");
  EXPECT_EQ(line_id["x=1"], "W4");
  EXPECT_FALSE(sh.shell().low_confidence);

  // A second, collinear plane on the first wall (new, larger id) joins its line: same id, still 4 walls.
  LiveMap m;
  for (auto& r : poly_walls(kBox, 0.0, 1.0)) m.add_plane(r);
  auto extra = poly_walls(kBox, 0.0, 0.3)[0];
  extra.id = "w0b";
  extra.center[0] += 1.0;
  m.add_plane(extra);
  ids["w0b"] = 5;
  sh.update(m, &ids);
  ASSERT_EQ(sh.shell().walls.size(), 4u);
  for (auto& w : sh.shell().walls) EXPECT_EQ(w.id, line_id[line_key(w)]) << line_key(w);
}

TEST(Understanding, ShellSelfNumbersFirstSeenWithoutEngineMap) {
  RoomShell sh;
  LiveMap m;
  auto walls = poly_walls(kBox);
  m.add_plane(walls[2]);  // w2 (line z=2) seen first
  sh.update(m, nullptr);
  m.add_plane(walls[0]);
  m.add_plane(walls[1]);
  m.add_plane(walls[3]);
  sh.update(m, nullptr);
  std::map<std::string, std::string> id_by_line;
  for (auto& w : sh.shell().walls) id_by_line[line_key(w)] = w.id;
  ASSERT_EQ(id_by_line.size(), 4u);
  EXPECT_EQ(id_by_line.at("z=2"), "W1");  // first seen, never renumbered
}

TEST(Understanding, ShellInferredWallsAreFlagged) {
  LiveMap m;
  std::vector<std::string> order;
  for (auto& r : poly_walls(kBox, 0.0, 1.0, {3})) m.add_plane(r), order.push_back(r.id);  // west wall (x = 1) unseen
  auto ids = engine_ids(order);
  RoomShell sh;
  sh.update(m, &ids);
  const auto& s = sh.shell();
  ASSERT_EQ(s.walls.size(), 4u);
  EXPECT_TRUE(s.closed);
  EXPECT_TRUE(s.low_confidence);
  EXPECT_LT(corner_err(s, kBox, 0.0), 0.25);
  int inferred = 0;
  std::string first;
  for (auto& w : s.walls) {
    if (!w.inferred) {
      EXPECT_EQ(w.id[0], 'W');
      continue;
    }
    ++inferred;
    first = w.id;
    EXPECT_EQ(w.id[0], 'I');
    EXPECT_NEAR(w.x0, 1.0, 0.25);
    EXPECT_NEAR(w.x1, 1.0, 0.25);  // the unseen west side
  }
  EXPECT_EQ(inferred, 1);
  sh.update(m, &ids);  // the inferred id is persistent across updates
  for (auto& w : sh.shell().walls)
    if (w.inferred) EXPECT_EQ(w.id, first);
}

TEST(Understanding, ShellEmptyAndTinyInputs) {
  RoomShell sh;
  LiveMap m;
  sh.update(m);
  EXPECT_TRUE(sh.shell().walls.empty());
  EXPECT_TRUE(sh.shell().corners.empty());
  EXPECT_TRUE(sh.shell().ceiling_inferred);
  // One wall alone and no cameras: no outline, the observed wall is still listed, low confidence.
  m.add_plane(poly_walls(kBox)[0]);
  sh.update(m);
  EXPECT_FALSE(sh.shell().closed);
  EXPECT_TRUE(sh.shell().low_confidence);
  EXPECT_EQ(sh.shell().walls.size(), 1u);
}

TEST(Understanding, ShellCeilingHeight) {
  LiveMap m;
  for (auto& r : poly_walls(kBox)) m.add_plane(r);
  RoomShell sh;
  sh.update(m);
  EXPECT_TRUE(sh.shell().ceiling_inferred);
  EXPECT_NEAR(sh.shell().ceiling_h, 2.5, 1e-9);
  m.add_plane(plane("f", "floor", {3.5, 0.1, 0}, {0, 1, 0}, {1, 0, 0}, 5, 4));
  m.add_plane(plane("c", "ceiling", {3.5, 2.7, 0}, {0, -1, 0}, {1, 0, 0}, 5, 4));
  sh.update(m);
  EXPECT_FALSE(sh.shell().ceiling_inferred);
  EXPECT_NEAR(sh.shell().ceiling_h, 2.6, 1e-9);
  m.add_plane(plane("c", "ceiling", {3.5, 9.0, 0}, {0, -1, 0}, {1, 0, 0}, 5, 4));  // a 9 m "ceiling" is not trusted
  sh.update(m);
  EXPECT_TRUE(sh.shell().ceiling_inferred);
}

TEST(Understanding, ShellCeilingUsesFloorPriorWithoutFloorPlane) {
  LiveMap m;
  m.add_camera({2.0, 2.0, 0.0});  // floor prior = camera y - 1.4 = 0.6
  m.add_plane(plane("c", "ceiling", {3.5, 3.4, 0}, {0, -1, 0}, {1, 0, 0}, 5, 4));
  ASSERT_FALSE(m.has_floor());
  RoomShell sh;
  sh.update(m);
  EXPECT_FALSE(sh.shell().ceiling_inferred);
  EXPECT_NEAR(sh.shell().ceiling_h, 3.4 - 0.6, 1e-9);
}

TEST(Understanding, ShellEarlyReturnsClearStaleOpeningIds) {
  Opening o;
  o.wall = "W9";
  RoomShell sh;
  LiveMap m;
  sh.update(m, nullptr, {o});  // no walls: early return
  ASSERT_EQ(sh.openings().size(), 1u);
  EXPECT_TRUE(sh.openings()[0].wall.empty());
}

TEST(Understanding, ShellInferredIdSurvivesYawFlipNear45Degrees) {
  RoomShell sh;
  auto ids = engine_ids({"w0", "w1", "w2"});
  std::string first;
  // 0.77 rad: yaw ~ +0.77. 0.80 rad: the Manhattan yaw wraps to ~ -0.77 and the frame's sides renumber.
  for (double rot : {0.77, 0.80, 0.77, 0.80}) {
    LiveMap m;
    for (auto& r : poly_walls(kBox, rot, 1.0, {3})) m.add_plane(r);  // west side never seen
    sh.update(m, &ids);
    if (rot == 0.80) EXPECT_LT(sh.yaw(), 0.0);
    std::string got;
    int n_inferred = 0;
    for (auto& w : sh.shell().walls)
      if (w.inferred) got = w.id, ++n_inferred;
    ASSERT_EQ(n_inferred, 1) << "rot " << rot;
    if (first.empty()) first = got;
    EXPECT_EQ(got, first) << "rot " << rot;
  }
}

TEST(Understanding, ShellAttachesOpeningsToWalls) {
  LiveMap m;
  std::vector<std::string> order;
  for (auto& r : poly_walls(kBox)) m.add_plane(r), order.push_back(r.id);
  auto ids = engine_ids(order);
  Opening door, win, stray;
  door.x0 = 2.0, door.z0 = -2.0, door.x1 = 3.0, door.z1 = -2.0;  // on the z = -2 wall (w0 -> W1)
  win.kind = OpeningKind::Window;
  win.x0 = 6.0, win.z0 = -1.0, win.x1 = 6.0, win.z1 = 0.0;       // on x = 6 (w1 -> W2)
  stray.x0 = 3.0, stray.z0 = 0.0, stray.x1 = 4.0, stray.z1 = 0.0;  // mid-room
  RoomShell sh;
  sh.update(m, &ids, {door, win, stray});
  ASSERT_EQ(sh.openings().size(), 3u);
  EXPECT_EQ(sh.openings()[0].wall, "W1");
  EXPECT_EQ(sh.openings()[1].wall, "W2");
  EXPECT_EQ(sh.openings()[1].kind, OpeningKind::Window);
  EXPECT_TRUE(sh.openings()[2].wall.empty());
}

TEST(Understanding, ShellIgnoresNonManhattanAndSmallWalls) {
  LiveMap m;
  for (auto& r : poly_walls(kBox)) m.add_plane(r);
  // A 45 degree partition wall and a picture frame: neither bends the box outline.
  m.add_plane(plane("diag", "wall", {3.5, 1.2, 0.0}, Vec3(1, 0, -1).normalized(), Vec3(1, 0, 1).normalized(), 2.0, 2.0));
  m.add_plane(plane("frame", "wall", {3.0, 1.5, -1.9}, {0, 0, 1}, {1, 0, 0}, 0.4, 0.3));
  auto ids = engine_ids({"w0", "w1", "w2", "w3", "diag", "frame"});
  RoomShell sh;
  sh.update(m, &ids);
  EXPECT_EQ(sh.shell().walls.size(), 4u);
  EXPECT_TRUE(sh.shell().closed);
  EXPECT_TRUE(sh.shell().low_confidence);  // the big diagonal wall is not explained by the outline
  EXPECT_LT(corner_err(sh.shell(), kBox, 0.0), 0.25);
}

TEST(Understanding, ShellIsDeterministic) {
  LiveMap m;
  for (auto& r : poly_walls(kL, 0.3)) m.add_plane(r);
  RoomShell a, b;
  a.update(m), b.update(m), b.update(m);
  ASSERT_EQ(a.shell().walls.size(), b.shell().walls.size());
  for (size_t i = 0; i < a.shell().walls.size(); ++i) {
    EXPECT_EQ(a.shell().walls[i].id, b.shell().walls[i].id);
    EXPECT_EQ(a.shell().walls[i].x0, b.shell().walls[i].x0);
    EXPECT_EQ(a.shell().walls[i].z1, b.shell().walls[i].z1);
  }
}

// ================================================================================================================
// Step 7: virtual coverage grids, GapFinder, LandmarkLabeler, sim gap metrics. Thresholds are synthetic hypotheses.
// ================================================================================================================
#include "capturekit/guide/gap_finder.hpp"
#include "capturekit/guide/landmark.hpp"

namespace {

geometry::Intrinsics sim_K() {
  geometry::Intrinsics K;
  K.width = 640, K.height = 480, K.fx = K.fy = 500, K.cx = 319.5, K.cy = 239.5;
  return K;
}

/// A vertical wall plane from (x0,z0) to (x1,z1), inward normal = (-dz, dx) (CCW rooms), height 2.5.
bundle::PlaneRecord wall_plane(const std::string& id, double x0, double z0, double x1, double z1) {
  const double L = std::hypot(x1 - x0, z1 - z0);
  const Vec3 u((x1 - x0) / L, 0, (z1 - z0) / L), n(-u.z(), 0, u.x());
  return plane(id, "wall", Vec3(0.5 * (x0 + x1), 1.25, 0.5 * (z0 + z1)), n, u, L, 2.5);
}

/// The box room (x 1..6, z -2..2) as wall planes, optionally with floor + ceiling planes.
LiveMap box_map(bool floor_ceiling, const std::vector<int>& skip = {}) {
  LiveMap m;
  for (auto& r : poly_walls(kBox, 0.0, 1.0, skip)) m.add_plane(r);
  if (floor_ceiling) {
    m.add_plane(plane("f", "floor", {3.5, 0.0, 0}, {0, 1, 0}, {1, 0, 0}, 5, 4));
    m.add_plane(plane("c", "ceiling", {3.5, 2.5, 0}, {0, -1, 0}, {1, 0, 0}, 5, 4));
  }
  return m;
}

Obs obs_at(const Vec3& p, const Vec3& look_at, double dyaw = 0, double dpitch = 0) {
  Obs o;
  const double yaw = std::atan2(look_at.x() - p.x(), look_at.z() - p.z()) + dyaw;
  const double hd = std::hypot(look_at.x() - p.x(), look_at.z() - p.z());
  const double pitch = std::atan2(look_at.y() - p.y(), hd) + dpitch;
  o.pose = pose_from_yaw_pitch(p, yaw, pitch);
  o.K = sim_K(), o.t_ns = 1000000000, o.sharp_norm = 1.0, o.keyframe = true;
  return o;
}

std::vector<Gap> gaps_of(const LiveMap& m, const Coverage* cov = nullptr, const Coverage* vcov = nullptr,
                         const std::vector<Object>* objs = nullptr, const LandmarkContext& user = {}) {
  RoomShell sh;
  sh.update(m);
  GapInputs in;
  in.shell = &sh.shell(), in.map = &m, in.cov = cov, in.vcov = vcov, in.objects = objs, in.user = user;
  return GapFinder().find(in);
}

int count_type(const std::vector<Gap>& g, GapType t) {
  return int(std::count_if(g.begin(), g.end(), [&](const Gap& x) { return x.type == t; }));
}

}  // namespace

TEST(Understanding, GapsFullRoomHasNone) {
  LiveMap m = box_map(true);
  auto g = gaps_of(m);
  EXPECT_TRUE(g.empty()) << "first: " << (g.empty() ? "" : g[0].why);
}

TEST(Understanding, GapMissingWallIsTheInferredSide) {
  LiveMap m = box_map(true, {3});  // west wall (x = 1) never seen
  auto g = gaps_of(m);
  ASSERT_EQ(g.size(), 1u);
  EXPECT_EQ(g[0].type, GapType::MissingWall);
  EXPECT_NEAR(g[0].x0, 1.0, 0.25);
  EXPECT_NEAR(g[0].x1, 1.0, 0.25);
  EXPECT_GT(g[0].pri, 0.8);
  EXPECT_EQ(g[0].why, "an unseen wall");  // no user pose: neutral wording
  LandmarkContext u;
  u.has_user = true, u.ux = 3.5, u.uz = 0, u.yaw = 0;
  auto g2 = gaps_of(m, nullptr, nullptr, nullptr, u);
  ASSERT_EQ(g2.size(), 1u);
  EXPECT_EQ(g2[0].why, "the wall on your right");  // facing +Z, +X is on the left, so x = 1 is on the right
}

TEST(Understanding, GapUnseenCornerOnlyWhereWallsStopShort) {
  auto build = [](double trim) {
    LiveMap m;
    // CCW box x 1..6, z -2..2; the corner (1,-2) is where w3 ends and w0 starts. `trim` m is cut off both there.
    m.add_plane(wall_plane("w0", 1.0 + trim, -2.0, 6.0, -2.0));
    m.add_plane(wall_plane("w1", 6.0, -2.0, 6.0, 2.0));
    m.add_plane(wall_plane("w2", 6.0, 2.0, 1.0, 2.0));
    m.add_plane(wall_plane("w3", 1.0, 2.0, 1.0, -2.0 + trim));
    m.add_plane(plane("f", "floor", {3.5, 0.0, 0}, {0, 1, 0}, {1, 0, 0}, 5, 4));
    m.add_plane(plane("c", "ceiling", {3.5, 2.5, 0}, {0, -1, 0}, {1, 0, 0}, 5, 4));
    return m;
  };
  LiveMap full = build(0.0), cut = build(0.6);
  EXPECT_TRUE(gaps_of(full).empty());
  auto g = gaps_of(cut);
  ASSERT_EQ(g.size(), 1u);
  EXPECT_EQ(g[0].type, GapType::UnseenCorner);
  EXPECT_NEAR(0.5 * (g[0].x0 + g[0].x1), 1.0, 0.1);
  EXPECT_NEAR(0.5 * (g[0].z0 + g[0].z1), -2.0, 0.1);
  EXPECT_EQ(g[0].why, "an unseen corner");
}

TEST(Understanding, GapCeilingAndFloorWithoutPlanes) {
  LiveMap m = box_map(false);
  auto g = gaps_of(m);
  EXPECT_EQ(count_type(g, GapType::UnseenCeiling), 1);
  EXPECT_EQ(count_type(g, GapType::UnseenFloor), 1);
  EXPECT_EQ(g.size(), 2u);
}

TEST(Understanding, VirtualCoverageClosesAMissingWallGap) {
  LiveMap m = box_map(true, {3});
  RoomShell sh;
  sh.update(m);
  const auto vs = GapFinder::virtual_surfaces(sh.shell(), m);
  ASSERT_EQ(vs.size(), 1u);  // only the inferred west wall (floor and ceiling planes exist)
  EXPECT_EQ(vs[0].id[0], 'v');
  EXPECT_GT(vs[0].n.x(), 0.9);  // faces the room (+x)
  GuideParams gp;
  Coverage vcov(gp);
  vcov.sync_virtual(vs);
  EXPECT_EQ(vcov.stats().valid, 0u);  // virtual grids never count towards completeness
  EXPECT_TRUE(vcov.deficient().empty());
  EXPECT_TRUE(vcov.plane_ids().empty());
  EXPECT_NEAR(vcov.stats(vs[0].id).seen_fraction(), 0.0, 1e-9);
  ASSERT_GT(vcov.stats(vs[0].id).valid, 500u);
  EXPECT_EQ(gaps_of(m, nullptr, &vcov).size(), 1u);  // nobody looked yet
  // Look at the west wall from the far side, a few yaw / pitch offsets.
  for (double dy : {-0.25, 0.0, 0.25})
    for (double dp : {-0.18, 0.18}) vcov.observe(obs_at({5.5, 1.25, 0.0}, {1.0, 1.25, 0.0}, dy, dp), nullptr, nullptr);
  EXPECT_GT(vcov.stats(vs[0].id).seen_fraction(), 0.8);
  EXPECT_TRUE(gaps_of(m, nullptr, &vcov).empty());  // looked at: no gap (open doorway / glass must not stall)
  // A real map sync keeps the virtual grid; dropping it from the set removes it.
  vcov.sync(m);
  EXPECT_GT(vcov.stats(vs[0].id).valid, 500u);
  vcov.sync_virtual({});
  EXPECT_EQ(vcov.stats(vs[0].id).valid, 0u);
}

TEST(Understanding, VirtualSurfacesNeedAShellAndRespectPlanes) {
  LiveMap empty;
  RoomShell sh;
  sh.update(empty);
  EXPECT_TRUE(GapFinder::virtual_surfaces(sh.shell(), empty).empty());
  LiveMap m = box_map(false);
  sh.update(m);
  const auto vs = GapFinder::virtual_surfaces(sh.shell(), m);  // closed room, no ceiling, no floor
  std::vector<std::string> ids;
  for (auto& v : vs) ids.push_back(v.id);
  EXPECT_EQ(ids, (std::vector<std::string>{"v:ceiling", "v:floor"}));
  ASSERT_EQ(vs.size(), 2u);
  EXPECT_NEAR(vs[0].c.y(), 2.5, 1e-9);
  EXPECT_LT(vs[0].n.y(), -0.9);
  EXPECT_NEAR(vs[0].eu, 5.0, 0.3);
}

TEST(Understanding, GapOccludedBehindAnObject) {
  LiveMap m = box_map(true);
  GuideParams gp;
  Coverage cov(gp);
  cov.sync(m);
  Object sofa;
  sofa.id = 1, sofa.cls = ObjectClass::Couch, sofa.centre = Vec3(3.5, 0.4, 0.0), sofa.size = Vec3(0.5, 0.8, 0.5);
  std::vector<Object> objs{sofa};
  auto g = gaps_of(m, &cov, nullptr, &objs);
  ASSERT_EQ(count_type(g, GapType::Occluded), 1) << "nothing observed: the floor around the sofa is unseen";
  for (auto& x : g)
    if (x.type == GapType::Occluded) EXPECT_EQ(x.why, "around the sofa");
  LandmarkContext u;
  u.has_user = true, u.ux = 1.5, u.uz = 0, u.yaw = kPi / 2;
  for (auto& x : gaps_of(m, &cov, nullptr, &objs, u))
    if (x.type == GapType::Occluded) EXPECT_TRUE(x.why == "behind the sofa" || x.why == "beside the sofa") << x.why;
  // Look at the floor around it from four sides: no Occluded gap any more.
  const Vec3 look(3.5, 0.0, 0.0);
  for (const Vec3& p : {Vec3(1.5, 1.4, 0), Vec3(5.5, 1.4, 0), Vec3(3.5, 1.4, 1.9), Vec3(3.5, 1.4, -1.9)})
    cov.observe(obs_at(p, look), nullptr, nullptr);
  EXPECT_EQ(count_type(gaps_of(m, &cov, nullptr, &objs), GapType::Occluded), 0);
  // No coverage = no evidence = no claim.
  EXPECT_EQ(count_type(gaps_of(m, nullptr, nullptr, &objs), GapType::Occluded), 0);
}

TEST(Understanding, GapsAreDeterministicSortedAndCapped) {
  LiveMap m = box_map(false, {3});
  auto a = gaps_of(m), b = gaps_of(m);
  ASSERT_EQ(a.size(), b.size());
  ASSERT_GE(a.size(), 2u);
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].type, b[i].type);
    EXPECT_EQ(a[i].x0, b[i].x0);
    EXPECT_EQ(a[i].why, b[i].why);
    if (i) EXPECT_GE(a[i - 1].pri, a[i].pri);
  }
  EXPECT_EQ(a[0].type, GapType::MissingWall);
  EXPECT_LE(a.size(), GapParams{}.max_gaps);
}

TEST(Understanding, LandmarkPhrases) {
  LandmarkLabeler L;
  std::vector<Opening> ops(2);
  ops[0].kind = OpeningKind::Window, ops[0].x0 = 6, ops[0].z0 = -1, ops[0].x1 = 6, ops[0].z1 = 0;
  ops[1].kind = OpeningKind::Door, ops[1].x0 = 2, ops[1].z0 = 2, ops[1].x1 = 3, ops[1].z1 = 2;
  Gap g;
  g.type = GapType::Weak, g.x0 = 6, g.z0 = -2, g.x1 = 6, g.z1 = 2;
  EXPECT_EQ(L.label(g, ops, {}, {}), "the window wall");
  g.x0 = 1, g.z0 = 2, g.x1 = 6, g.z1 = 2;  // centre (3.5, 2): the door at 2..3 is 0.5 m away
  EXPECT_EQ(L.label(g, ops, {}, {}), "the door wall");
  g.type = GapType::UnseenCorner, g.x0 = 5.7, g.z0 = -2.3, g.x1 = 6.3, g.z1 = -1.7;
  EXPECT_EQ(L.label(g, {}, {}, {}), "an unseen corner");
  LandmarkContext u;
  u.has_user = true, u.ux = 3, u.uz = 0, u.yaw = 0;  // facing +Z, +X is on the left
  EXPECT_EQ(L.label(g, {}, {}, u), "the corner on your left");  // corner (6, -2): +x, and 2 m behind: |rel| = 56 deg
  g.type = GapType::UnseenCeiling;
  EXPECT_EQ(L.label(g, {}, {}, u), "the ceiling");
  g.type = GapType::UnseenFloor;
  EXPECT_EQ(L.label(g, {}, {}, u), "the floor");
  EXPECT_EQ(L.direction({}, 1, 1), "");
  EXPECT_EQ(L.direction(u, 3, 5), "ahead");
  EXPECT_EQ(L.direction(u, 3, -5), "behind you");
  EXPECT_EQ(L.direction(u, 8, 0), "on your left");
  EXPECT_EQ(L.direction(u, -2, 0), "on your right");
  EXPECT_STREQ(LandmarkLabeler::class_phrase(ObjectClass::Couch), "sofa");
  EXPECT_STREQ(LandmarkLabeler::class_phrase(ObjectClass::Unknown), "furniture");
  // Every phrase, for every type, user known or not, is at most 8 words and never empty.
  std::vector<Object> objs(1);
  objs[0].cls = ObjectClass::DiningTable, objs[0].centre = Vec3(3, 0.4, 0);
  for (int t = 0; t <= int(GapType::Weak); ++t)
    for (bool user : {false, true}) {
      Gap x;
      x.type = GapType(t), x.x0 = 2, x.z0 = -1, x.x1 = 4, x.z1 = 1;
      LandmarkContext c = user ? u : LandmarkContext{};
      const std::string s = L.label(x, ops, objs, c);
      EXPECT_FALSE(s.empty());
      EXPECT_LE(std::count(s.begin(), s.end(), ' ') + 1, 8) << s;
    }
}

namespace {
/// Removes the top-level "understanding":{...} block of a sim report.
std::string without_block(std::string r) {
  auto a = r.find(",\"understanding\":{");
  if (a == std::string::npos) a = r.find("\"understanding\":{");
  if (a == std::string::npos) return r;
  size_t b = r.find('{', a), depth = 0;
  for (; b < r.size(); ++b) {
    depth += r[b] == '{', depth -= r[b] == '}';
    if (depth == 0) break;
  }
  r.erase(a, b + 1 - a);
  return r;
}

double block_num(const std::string& r, const char* key) {
  auto o = r.find("\"understanding\":{");
  auto at = r.find(std::string("\"") + key + "\":", o);
  return at == std::string::npos ? -1 : std::atof(r.c_str() + at + std::strlen(key) + 3);
}

struct GapRun {
  std::string room;
  uint64_t seed;
  double max_s;
};

std::vector<std::string> run_gap_sims(const std::vector<GapRun>& runs, bool eval) {
  std::vector<std::string> out(runs.size());
  std::vector<std::thread> th;
  for (size_t k = 0; k < runs.size(); ++k)
    th.emplace_back([&, k] {
      sim::Config c;
      c.room = runs[k].room, c.seed = runs[k].seed, c.max_s = runs[k].max_s;
      c.guide.understanding = 1;
      c.human.hold_level = true;
      c.view_points = true;
      c.detector_oracle = true;
      c.understanding_eval = eval;
      out[k] = sim::simulate(c);
    });
  for (auto& t : th) t.join();
  return out;
}
}  // namespace

TEST(Understanding, SimGapHarnessIsInertAndDeterministic) {
  const std::vector<GapRun> runs = {{"box", 1, 45}, {"objects", 1, 45}};
  auto on = run_gap_sims(runs, true), off = run_gap_sims(runs, false), on2 = run_gap_sims(runs, true);
  for (size_t k = 0; k < runs.size(); ++k) {
    EXPECT_NE(on[k].find("\"understanding\":{"), std::string::npos);
    EXPECT_EQ(off[k].find("\"understanding\":{"), std::string::npos);
    EXPECT_EQ(without_block(on[k]), off[k]) << runs[k].room << ": the shadow stages must not change the sim";
    EXPECT_EQ(on[k], on2[k]) << runs[k].room << ": deterministic";
    EXPECT_GE(block_num(on[k], "checkpoints"), 2) << runs[k].room;
  }
}

// Recall of hidden regions and false-gap rate vs the sim's own ground truth. SYNTHETIC: every threshold is a hypothesis
// (Gate 0); real rooms need real bundles. The table is copied into capturekit/docs/reports/understanding_synth.md.
TEST(Understanding, SimGapRecallAndFalseGapTable) {
  std::vector<GapRun> runs;
  for (const char* room : {"box", "L", "objects", "door_window"})
    for (uint64_t s : {1, 2, 3}) runs.push_back({room, s, 90});
  const auto t0 = std::chrono::steady_clock::now();
  const auto reps = run_gap_sims(runs, true);
  const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  auto sub = [](const std::string& r, const char* kind, const char* group, const char* f) {
    auto o = r.find("\"understanding\":{");
    auto g = r.find(std::string("\"") + group + "\":{", o);
    auto k = r.find(std::string("\"") + kind + "\":{", g);
    auto at = r.find(std::string("\"") + f + "\":", k);
    return std::atoi(r.c_str() + at + std::strlen(f) + 3);
  };
  int far_total = 0;
  std::map<std::string, std::array<int, 2>> truth, gaps;  // kind -> {n, hit} / {n, false}
  std::cout << "\n  Understanding.SimGapRecallAndFalseGapTable (planner, hold_level, view_points, understanding on, "
               "checkpoint every 15 s to 90 s), wall "
            << wall_s << " s\n";
  for (size_t k = 0; k < runs.size(); ++k) {
    const auto& r = reps[k];
    std::cout << "  " << runs[k].room << " seed " << runs[k].seed << ": truth " << block_num(r, "truth_hit") << "/"
              << block_num(r, "truth_n") << " found, gaps " << block_num(r, "gaps_n") << " (" << block_num(r, "gaps_false")
              << " false)\n";
    for (const char* kd : {"wall", "corner", "block", "ceiling", "floor"})
      truth[kd][0] += sub(r, kd, "truth", "n"), truth[kd][1] += sub(r, kd, "truth", "hit");
    for (const char* kd : {"missing_wall", "unseen_corner", "unseen_ceiling", "unseen_floor", "occluded", "weak"})
      gaps[kd][0] += sub(r, kd, "gaps", "n"), gaps[kd][1] += sub(r, kd, "gaps", "false"), far_total += sub(r, kd, "gaps", "far");
  }
  int tn = 0, th = 0, gn = 0, gf = 0;
  std::cout << "  | hidden region | truth | found | recall |\n  |---|---|---|---|\n";
  for (auto& [kd, v] : truth) {
    tn += v[0], th += v[1];
    std::cout << "  | " << kd << " | " << v[0] << " | " << v[1] << " | " << (v[0] ? double(v[1]) / v[0] : 0.0) << " |\n";
  }
  std::cout << "  | gap type | gaps | false | false rate |\n  |---|---|---|---|\n";
  for (auto& [kd, v] : gaps) {
    gn += v[0], gf += v[1];
    std::cout << "  | " << kd << " | " << v[0] << " | " << v[1] << " | " << (v[0] ? double(v[1]) / v[0] : 0.0) << " |\n";
  }
  std::cout << "  missing_wall false gaps farther than 1 m from any true wall: " << far_total << "\n";
  const double recall = tn ? double(th) / tn : 0.0, frate = gn ? double(gf) / gn : 1.0;
  std::cout << "  overall: recall " << th << "/" << tn << " = " << recall << ", false gaps " << gf << "/" << gn << " = " << frate
            << "\n";
  ASSERT_GT(tn, 0) << "no hidden truth regions at all: the scenario would prove nothing";
  ASSERT_GT(gn, 0);
  // HYPOTHESIS thresholds (synthetic, Gate 0): set just inside the first measurement (recall 0.23, false 0.05, see
  // docs/reports/understanding_synth.md); they guard against regressions, they are not product targets.
  EXPECT_GE(recall, 0.15);
  EXPECT_LE(frate, 0.15);
}

// ---- step 5: GuideEngine::on_detections (recorded detections -> live objects) ----
namespace {

bundle::PoseSample pose_sample(int64_t t, const Vec3& p, double yaw, double pitch) {
  auto T = pose_from_yaw_pitch(p, yaw, pitch);
  bundle::PoseSample s;
  s.t_ns = t;
  for (int i = 0; i < 3; ++i) s.t_world_from_camera[i] = T.translation()[i];
  auto q = T.se3().unit_quaternion();
  s.q_world_from_camera_wxyz[0] = q.w(), s.q_world_from_camera_wxyz[1] = q.x();
  s.q_world_from_camera_wxyz[2] = q.y(), s.q_world_from_camera_wxyz[3] = q.z();
  return s;
}

const Vec3 kCam{2.5, 1.4, 0.0};
const double kPitch = deg2rad(-10);

/// Intrinsics + floor + back wall + poses every 50 ms for 1 s, as the engine would see them from a session.
void feed_scene(GuideEngine& e) {
  bundle::IntrinsicsSample k;
  k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 319.5, k.cy = 239.5;
  e.on_intrinsics(k);
  e.on_plane(plane("f", "floor", {2.5, 0, 2}, {0, 1, 0}, {1, 0, 0}, 6, 6));
  e.on_plane(plane("w1", "wall", {2.5, 1.25, 3.5}, {0, 0, -1}, {1, 0, 0}, 5, 2.5));
  for (int64_t i = 0; i <= 20; ++i) e.on_pose(pose_sample(i * 50'000'000, kCam, 0.0, kPitch));
}

const Object kChair = make_obj(ObjectClass::Chair, {2.5, 0.45, 3.0}, {0.5, 0.9, 0.5}, 0.0);

bundle::DetectionsRecord chair_record(int frame, bool also_junk = false) {
  LiftContext ctx = lift_ctx(kCam, 0.0, kPitch);
  Detection d = project_box(kChair, ctx);
  bundle::DetectionsRecord r;
  r.t_ns = int64_t(frame) * 100'000'000, r.frame_id = uint32_t(frame);
  r.boxes.push_back({int32_t(ObjectClass::Chair), 0.8f, d.x0, d.y0, d.x1, d.y1});
  if (also_junk) {  // out-of-range class, non-finite box, unknown class: all ignored
    r.boxes.push_back({99, 0.9f, d.x0, d.y0, d.x1, d.y1});
    r.boxes.push_back({-3, 0.9f, d.x0, d.y0, d.x1, d.y1});
    r.boxes.push_back({0, 0.9f, d.x0, d.y0, d.x1, d.y1});
    r.boxes.push_back({int32_t(ObjectClass::Couch), std::nanf(""), d.x0, d.y0, d.x1, d.y1});
  }
  return r;
}

GuideParams understanding_params(double on) {
  GuideParams gp;
  gp.understanding = on;
  return gp;
}

}  // namespace

TEST(Understanding, EngineLiftsRecordedDetectionsToObjects) {
  GuideEngine e(understanding_params(1));
  feed_scene(e);
  e.on_detections(chair_record(1, true));
  e.on_detections(chair_record(2));
  EXPECT_TRUE(e.objects().empty()) << "floor-ray-only tracks need 3 observations";
  EXPECT_EQ(e.overlay(1'000'000'000).to_json().find("\"objects\""), std::string::npos);
  e.on_detections(chair_record(3));
  ASSERT_EQ(e.objects().size(), 1u);
  EXPECT_EQ(e.objects()[0].cls, ObjectClass::Chair);
  EXPECT_LT(std::hypot(e.objects()[0].centre.x() - kChair.centre.x(), e.objects()[0].centre.z() - kChair.centre.z()), 0.4);
  // The object is an obstacle in the nav grid.
  int ix, iz;
  e.nav().to_cell({e.objects()[0].centre.x(), 0, e.objects()[0].centre.z()}, ix, iz);
  EXPECT_TRUE(e.nav().obstacle(ix, iz));
  // Overlay: one flat string "id,cls,x,z,w,d,yaw_deg,h".
  const std::string oj = e.overlay(1'000'000'000).to_json();
  EXPECT_NE(oj.find("\"objects\":\"1,chair,"), std::string::npos) << oj;
  auto a = json::parse_flat(oj);
  ASSERT_TRUE(a.has_value()) << oj;  // still flat JSON
  EXPECT_LT(oj.size(), 4096u);
  EXPECT_NE(e.report_json().find("\"objects\":[{"), std::string::npos);
  // Deterministic: a second engine gives the identical overlay and report.
  GuideEngine f(understanding_params(1));
  feed_scene(f);
  for (int k = 1; k <= 3; ++k) f.on_detections(chair_record(k, k == 1));
  EXPECT_EQ(f.overlay(1'000'000'000).to_json(), oj);
  EXPECT_EQ(f.report_json(), e.report_json());
}

TEST(Understanding, EngineFlagOffIgnoresDetections) {
  GuideEngine off(understanding_params(0)), base(understanding_params(0));
  feed_scene(off);
  feed_scene(base);
  for (int k = 1; k <= 5; ++k) off.on_detections(chair_record(k));
  EXPECT_TRUE(off.objects().empty());
  EXPECT_EQ(off.report_json(), base.report_json());
  EXPECT_EQ(off.overlay(1'000'000'000).to_json(), base.overlay(1'000'000'000).to_json());
  EXPECT_EQ(off.overlay(1'000'000'000).to_json().find("\"objects\""), std::string::npos);
  // Flag on but nothing published (one frame): the report is the no-detection report.
  GuideEngine on(understanding_params(1)), none(understanding_params(1));
  feed_scene(on);
  feed_scene(none);
  on.on_detections(chair_record(1));
  EXPECT_EQ(on.report_json(), none.report_json());
  // Detections before intrinsics (with a pose track present, so the have_K_ check itself is reached) are ignored.
  GuideEngine early(understanding_params(1));
  for (int64_t i = 0; i <= 20; ++i) early.on_pose(pose_sample(i * 50'000'000, kCam, 0.0, kPitch));
  for (int k = 1; k <= 3; ++k) early.on_detections(chair_record(k));
  EXPECT_TRUE(early.objects().empty());
  bundle::IntrinsicsSample k;
  k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 319.5, k.cy = 239.5;
  early.on_intrinsics(k);
  early.on_plane(plane("f", "floor", {2.5, 0, 2}, {0, 1, 0}, {1, 0, 0}, 6, 6));
  early.on_detections(chair_record(1));
  early.on_detections(chair_record(2));
  EXPECT_TRUE(early.objects().empty()) << "the three early frames must not have counted";
  early.on_detections(chair_record(3));
  EXPECT_EQ(early.objects().size(), 1u);
}

TEST(Understanding, EngineDetectionTimeOffTheGridUsesTheNearestPose) {
  GuideEngine e(understanding_params(1));
  feed_scene(e);  // last pose at 1.0 s
  auto at = [](int frame, int64_t t) {
    auto r = chair_record(frame);
    r.t_ns = t;
    return r;
  };
  e.on_detections(at(1, 520'000'000));    // between poses (pose + 20 ms): interpolated
  e.on_detections(at(2, 1'010'000'000));  // past the back of the track (pose + 10 ms): nearest pose
  e.on_detections(at(3, 1'030'000'000));  // 30 ms past: still within 50 ms
  ASSERT_EQ(e.objects().size(), 1u);
  EXPECT_LT(std::hypot(e.objects()[0].centre.x() - kChair.centre.x(), e.objects()[0].centre.z() - kChair.centre.z()), 0.4);
  GuideEngine far(understanding_params(1));
  feed_scene(far);
  for (int k = 1; k <= 3; ++k) far.on_detections(at(k, 1'200'000'000 + k));  // 200 ms past the last pose: dropped
  EXPECT_TRUE(far.objects().empty());
}

TEST(Understanding, EngineMovedObjectLeavesNoStaleObstacleCells) {
  GuideEngine e(understanding_params(1));
  feed_scene(e);
  auto seen = [&](const Object& o, int from, int n) {
    LiftContext ctx = lift_ctx(kCam, 0.0, kPitch);
    for (int f = from; f < from + n; ++f) {
      Detection d = project_box(o, ctx);
      bundle::DetectionsRecord r;
      r.t_ns = int64_t(f) * 20'000'000, r.frame_id = uint32_t(f);
      r.boxes.push_back({int32_t(o.cls), 0.8f, d.x0, d.y0, d.x1, d.y1});
      e.on_detections(r);
    }
  };
  seen(kChair, 1, 3);
  ASSERT_EQ(e.objects().size(), 1u);
  const Vec3 first = e.objects()[0].centre;
  Object moved = kChair;
  moved.centre.x() += 0.5;  // same object (within the merge distance), now half a metre to the side
  seen(moved, 10, 20);
  ASSERT_EQ(e.objects().size(), 1u);
  EXPECT_GT((e.objects()[0].centre - first).norm(), 0.25) << "the track must have moved";
  // The obstacle layer equals a fresh grid stamped with the current objects only.
  GuideParams gp;
  NavGrid fresh(gp);
  fresh.set_floor_y(e.map().floor_y());
  for (auto& o : e.objects()) fresh.add_object_box(o);
  int diff = 0, n_obs = 0;
  for (double x = 1.0; x < 4.0; x += 0.05)
    for (double z = 1.5; z < 4.5; z += 0.05) {
      int ix, iz, jx, jz;
      e.nav().to_cell({x, 0, z}, ix, iz);
      fresh.to_cell({x, 0, z}, jx, jz);
      const bool a = e.nav().obstacle(ix, iz), b = fresh.obstacle(jx, jz);
      diff += a != b, n_obs += a;
    }
  EXPECT_EQ(diff, 0);
  EXPECT_GT(n_obs, 20);
}

TEST(Understanding, OverlayObjectsAreCappedAndStaySmall) {
  GuideEngine e(understanding_params(1));
  feed_scene(e);
  LiftContext ctx = lift_ctx(kCam, 0.0, kPitch);
  int frame = 1;
  for (int zi = 0; zi < 3; ++zi)
    for (int xi = 0; xi < 5; ++xi) {
      Object o = make_obj(ObjectClass::Chair, {2.5 + 0.9 * (xi - 2), 0.45, 2.5 + 1.0 * zi}, {0.5, 0.9, 0.5}, 0.0);
      for (int k = 0; k < 3; ++k) {
        Detection d = project_box(o, ctx);
        bundle::DetectionsRecord r;
        r.t_ns = 500'000'000, r.frame_id = uint32_t(frame++);
        r.boxes.push_back({int32_t(o.cls), 0.8f, d.x0, d.y0, d.x1, d.y1});
        e.on_detections(r);
      }
    }
  ASSERT_GT(e.objects().size(), OverlayModel::kMaxObjects);
  const std::string oj = e.overlay(1'000'000'000).to_json();
  const auto p = oj.find("\"objects\":\"");
  ASSERT_NE(p, std::string::npos) << oj;
  const std::string val = oj.substr(p + 11, oj.find('"', p + 11) - p - 11);
  EXPECT_EQ(size_t(std::count(val.begin(), val.end(), ';')) + 1, OverlayModel::kMaxObjects);
  EXPECT_LT(oj.size(), 4096u);
  EXPECT_TRUE(json::parse_flat(oj).has_value());
  EXPECT_EQ(val.substr(0, 8), "1,chair,");  // ordered by id
}

TEST(Understanding, OverlayObjectValuesAreClampedNotTruncated) {
  OverlayModel m;
  OverlayObject o;
  o.id = 2000000000, o.cls = "dining_table", o.x = 1e30, o.z = -1e30, o.w = 5e9, o.d = 0.9, o.h = -7e7, o.yaw_deg = 12;
  m.object_list.push_back(o);
  const std::string j = m.to_json();
  EXPECT_NE(j.find("\"objects\":\"2000000000,dining_table,10000.00,-10000.00,10000.00,0.90,12,-10000.00\""), std::string::npos) << j;
}

TEST(Understanding, OverlayObjectsKeyIsFlatBoundedAndFinite) {
  OverlayModel m;
  EXPECT_EQ(m.to_json().find("objects"), std::string::npos);
  for (int i = 0; i < 3; ++i) {
    OverlayObject o;
    o.id = i + 1, o.cls = "couch", o.x = 1.234 + i, o.z = -0.5, o.w = 1.8, o.d = 0.9, o.h = 0.85, o.yaw_deg = 30;
    m.object_list.push_back(o);
  }
  m.object_list[1].x = std::numeric_limits<double>::quiet_NaN();  // never emitted
  m.object_list[2].cls = "bad,cls";                               // delimiters never leak into the string
  const std::string j = m.to_json();
  EXPECT_NE(j.find("\"objects\":\"1,couch,1.23,-0.50,1.80,0.90,30,0.85\""), std::string::npos) << j;
  EXPECT_EQ(j.find(";"), std::string::npos);
  EXPECT_TRUE(json::parse_flat(j).has_value());
}
