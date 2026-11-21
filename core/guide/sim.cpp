#include "capturekit/guide/sim.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

#include "capturekit/guide/coverage.hpp"
#include "capturekit/guide/engine.hpp"
#include "capturekit/guide/gap_finder.hpp"
#include "capturekit/guide/object_lifter.hpp"
#include "capturekit/guide/room_shell.hpp"
#include "capturekit/quality/quality.hpp"
#include "capturekit/util/json.hpp"

namespace capturekit::guide::sim {

Room make_room(const std::string& name) {
  Room r;
  r.name = name;
  using C = ObjectClass;
  if (name == "furnished4") {  // T4 (ADR 0029): sofa on wall 0, wardrobe on wall 1, bed (head) on wall 3, wall 2 textureless
    r.poly = {{0, 0}, {5, 0}, {5, 4.5}, {0, 4.5}};
    r.rects = {{0, 0, 5, 4.5}};
    r.boxes = {{1.0, 0.0, 2.8, 0.9, 0.85, int(C::Couch)}, {4.4, 0.6, 5.0, 2.2, 2.0}, {0.0, 2.3, 2.0, 3.9, 0.55, int(C::Bed)}};
    r.start_x = 3.0, r.start_z = 2.6;
    r.arcore_planes = true, r.textureless_wall = 2;
    return r;
  }
  if (name == "furnishedL") {  // sofa on wall 0, bed (head) on wall 1, wardrobe on wall 5, wall 4 textureless
    r.poly = {{0, 0}, {6, 0}, {6, 3}, {3, 3}, {3, 5}, {0, 5}};
    r.rects = {{0, 0, 6, 3}, {0, 3, 3, 5}};
    r.boxes = {{1.5, 0.0, 3.3, 0.9, 0.85, int(C::Couch)}, {4.0, 0.8, 6.0, 2.4, 0.55, int(C::Bed)}, {0.0, 3.2, 0.6, 4.8, 2.0}};
    r.start_x = 1.5, r.start_z = 1.8;
    r.arcore_planes = true, r.textureless_wall = 4;
    return r;
  }
  if (name == "L") {
    r.poly = {{0, 0}, {6, 0}, {6, 3}, {3, 3}, {3, 5}, {0, 5}};
    r.rects = {{0, 0, 6, 3}, {0, 3, 3, 5}};
    r.start_x = 1.5, r.start_z = 1.5;
  } else {
    r.poly = {{0, 0}, {5, 0}, {5, 4}, {0, 4}};
    r.rects = {{0, 0, 5, 4}};
    r.start_x = 2.5, r.start_z = 2.0;
    if (name == "furniture") {
      r.boxes = {{0.3, 0.0, 2.1, 0.9, 0.8}, {2.3, 1.6, 3.5, 2.4, 0.75}, {4.4, 0.3, 5.0, 2.3, 2.0}};
      r.start_x = 1.0, r.start_z = 2.8;
    }
    if (name == "objects") {  // Phase 9 step 3: classed furniture for the detector oracle
      r.boxes = {{0.3, 0.1, 2.1, 0.7, 0.8, int(C::Couch)},
                 {3.3, 1.7, 4.5, 2.5, 0.75, int(C::DiningTable)},
                 {1.0, 3.0, 1.5, 3.5, 0.9, int(C::Chair)},
                 {4.4, 0.3, 4.8, 0.7, 0.7, int(C::PottedPlant)}};
      r.start_x = 2.0, r.start_z = 2.0;
    }
    if (name == "door_window") r.openings = {{0, 0, 1.0, 1.9, 0.0, 2.0}, {1, 1, 1.4, 2.6, 0.9, 1.9}};
  }
  return r;
}

namespace {

struct Surf {
  std::string kind;
  Vec3 o, u, v, n;
  double su = 0, sv = 0;
  int nu = 0, nv = 0;
  std::vector<CellStat> cells;
  std::vector<uint8_t> valid;
  // plane emission state: extent of seen points in (u,v)
  double umin = 1e9, umax = -1e9, vmin = 1e9, vmax = -1e9;
  int npts = 0;
  // arcore plane mode (walls): seen points per 0.25 m along-wall bin (count, v extent)
  std::vector<int> bin_n;
  std::vector<double> bin_vmin, bin_vmax;
  static constexpr double kBin = 0.25;
  Vec3 center(int i, int j, double c) const { return o + u * ((i + 0.5) * c) + v * ((j + 0.5) * c); }
  void note(double du, double dv) {
    umin = std::min(umin, du), umax = std::max(umax, du);
    vmin = std::min(vmin, dv), vmax = std::max(vmax, dv);
    ++npts;
    if (bin_n.empty()) return;
    const size_t b = size_t(std::clamp(int(du / kBin), 0, int(bin_n.size()) - 1));
    ++bin_n[b], bin_vmin[b] = std::min(bin_vmin[b], dv), bin_vmax[b] = std::max(bin_vmax[b], dv);
  }
};

struct Face {
  Vec3 o, u, v, n;
  double su, sv;
};

bool in_poly(const std::vector<std::pair<double, double>>& poly, double x, double z) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    auto [xi, zi] = poly[i];
    auto [xj, zj] = poly[j];
    if (((zi > z) != (zj > z)) && (x < (xj - xi) * (z - zi) / (zj - zi) + xi)) in = !in;
  }
  return in;
}

double dist_seg(double px, double pz, double ax, double az, double bx, double bz) {
  double dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
  double t = l2 > 0 ? std::clamp(((px - ax) * dx + (pz - az) * dz) / l2, 0.0, 1.0) : 0.0;
  return std::hypot(px - (ax + t * dx), pz - (az + t * dz));
}

/// 2D segment intersection parameter along (a->b) with segment (c->d); returns t in [0,1] or -1.
double seg_hit(double ax, double az, double bx, double bz, double cx, double cz, double dx, double dz) {
  double rx = bx - ax, rz = bz - az, sx = dx - cx, sz = dz - cz;
  double den = rx * sz - rz * sx;
  if (std::fabs(den) < 1e-12) return -1;
  double t = ((cx - ax) * sz - (cz - az) * sx) / den, u = ((cx - ax) * rz - (cz - az) * rx) / den;
  return (t >= 0 && t <= 1 && u >= 0 && u <= 1) ? t : -1;
}

bool seg_box(const Vec3& a, const Vec3& b, const Box& bx) {
  Vec3 d = b - a;
  double t0 = 0, t1 = 1;
  const double lo[3] = {bx.x0, 0.0, bx.z0}, hi[3] = {bx.x1, bx.h, bx.z1};
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(d[k]) < 1e-12) {
      if (a[k] < lo[k] || a[k] > hi[k]) return false;
    } else {
      double ta = (lo[k] - a[k]) / d[k], tb = (hi[k] - a[k]) / d[k];
      if (ta > tb) std::swap(ta, tb);
      t0 = std::max(t0, ta), t1 = std::min(t1, tb);
      if (t0 > t1) return false;
    }
  }
  return true;
}

struct World {
  Room room;
  double cell;
  std::vector<Surf> surfs;   // coverage targets: walls, floor, ceiling
  std::vector<Face> faces;   // point sources only (furniture sides)
  std::vector<Face> through;  // point sources seen only through an opening (0.5 m behind it), one per opening
  std::vector<double> weights;

  World(const Room& r, double c) : room(r), cell(c) {
    const auto& P = r.poly;
    for (size_t i = 0; i < P.size(); ++i) {
      auto [x0, z0] = P[i];
      auto [x1, z1] = P[(i + 1) % P.size()];
      double L = std::hypot(x1 - x0, z1 - z0);
      Surf s;
      s.kind = "wall";
      s.o = {x0, 0, z0};
      s.u = {(x1 - x0) / L, 0, (z1 - z0) / L};
      s.v = {0, 1, 0};
      s.n = {-(z1 - z0) / L, 0, (x1 - x0) / L};  // inward for a CCW polygon
      s.su = L, s.sv = r.height;
      add(std::move(s));
      Surf& ws = surfs.back();
      for (auto& op : r.openings) {  // no wall (and no coverage target) inside a door / window
        if (op.wall != int(i)) continue;
        for (int j = 0; j < ws.nv; ++j)
          for (int k = 0; k < ws.nu; ++k) {
            double cu = (k + 0.5) * cell, cv = (j + 0.5) * cell;
            if (cu > op.u0 && cu < op.u1 && cv > op.y0 && cv < op.y1) ws.valid[size_t(j) * size_t(ws.nu) + size_t(k)] = 0;
          }
        through.push_back({ws.o + ws.u * op.u0 - ws.n * 0.5 + Vec3(0, op.y0, 0), ws.u, {0, 1, 0}, ws.n, op.u1 - op.u0, op.y1 - op.y0});
      }
    }
    for (auto& rc : r.rects) {
      for (int ceil = 0; ceil < 2; ++ceil) {
        Surf s;
        s.kind = ceil ? "ceiling" : "floor";
        s.o = {rc[0], ceil ? r.height : 0.0, rc[1]};
        s.u = {1, 0, 0};
        s.v = {0, 0, 1};
        s.n = {0, ceil ? -1.0 : 1.0, 0};
        s.su = rc[2] - rc[0], s.sv = rc[3] - rc[1];
        add(std::move(s));
      }
    }
    for (auto& b : r.boxes) {
      faces.push_back({{b.x0, 0, b.z0}, {0, 0, 1}, {0, 1, 0}, {-1, 0, 0}, b.z1 - b.z0, b.h});
      faces.push_back({{b.x1, 0, b.z0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0}, b.z1 - b.z0, b.h});
      faces.push_back({{b.x0, 0, b.z0}, {1, 0, 0}, {0, 1, 0}, {0, 0, -1}, b.x1 - b.x0, b.h});
      faces.push_back({{b.x0, 0, b.z1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, b.x1 - b.x0, b.h});
      faces.push_back({{b.x0, b.h, b.z0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}, b.x1 - b.x0, b.z1 - b.z0});
    }
    for (auto& s : surfs) weights.push_back(s.su * s.sv);
    for (auto& f : faces) weights.push_back(f.su * f.sv);
    for (auto& f : through) weights.push_back(f.su * f.sv);
  }

  /// A point behind opening `k` is visible from `a` when the sight ray passes through that opening.
  bool through_visible(const Vec3& a, const Vec3& p, size_t k) const {
    const Face& f = through[k];
    const Vec3 wo = f.o + f.n * 0.5;  // the opening's corner on the wall plane
    const double den = (p - a).dot(f.n);
    if (std::fabs(den) < 1e-9) return false;
    const double t = (wo - a).dot(f.n) / den;
    if (t <= 0 || t >= 1) return false;
    const Vec3 hit = a + (p - a) * t;
    const double hu = (hit - wo).dot(f.u), hv = hit.y() - wo.y();
    return hu > 0 && hu < f.su && hv > 0 && hv < f.sv && los(a, hit);
  }

  /// Nearest hit of the ray a + t * dir (dir unit) over every point source. Wall cells with valid == 0 (doors,
  /// windows) let the ray pass; occlusion is the nearest-hit rule itself.
  struct Hit {
    double t = 1e18;
    Vec3 p, n;
    int surf = -1, face = -1;  // face >= 0: furniture face index; both < 0 with ok: a through face
    bool ok = false;
  };
  Hit raycast(const Vec3& a, const Vec3& dir) const {
    Hit h;
    auto rect = [&](const Vec3& o, const Vec3& u, const Vec3& v, const Vec3& n, double su, double sv, double& du,
                    double& dv) -> double {  // ray parameter of the hit inside the rectangle, or -1
      const double den = dir.dot(n);
      if (std::fabs(den) < 1e-12) return -1;
      const double t = (o - a).dot(n) / den;
      if (t <= 1e-6 || t >= h.t) return -1;
      const Vec3 q = a + dir * t - o;
      du = q.dot(u), dv = q.dot(v);
      return (du >= 0 && du <= su && dv >= 0 && dv <= sv) ? t : -1;
    };
    double du = 0, dv = 0;
    for (size_t i = 0; i < surfs.size(); ++i) {
      const Surf& s = surfs[i];
      const double t = rect(s.o, s.u, s.v, s.n, s.su, s.sv, du, dv);
      if (t < 0) continue;
      if (s.kind == "wall") {
        const int ci = std::min(s.nu - 1, int(du / cell)), cj = std::min(s.nv - 1, int(dv / cell));
        if (!s.valid[size_t(cj) * size_t(s.nu) + size_t(ci)]) continue;  // through a door / window
      }
      h.t = t, h.p = a + dir * t, h.n = s.n, h.surf = int(i), h.face = -1, h.ok = true;
    }
    for (size_t i = 0; i < faces.size(); ++i) {
      const Face& f = faces[i];
      const double t = rect(f.o, f.u, f.v, f.n, f.su, f.sv, du, dv);
      if (t < 0) continue;
      h.t = t, h.p = a + dir * t, h.n = f.n, h.surf = -1, h.face = int(i), h.ok = true;
    }
    for (const Face& f : through) {
      const double t = rect(f.o, f.u, f.v, f.n, f.su, f.sv, du, dv);
      if (t < 0) continue;
      h.t = t, h.p = a + dir * t, h.n = f.n, h.surf = -1, h.face = -1, h.ok = true;
    }
    return h;
  }

  void add(Surf s) {
    s.nu = std::max(1, int(std::ceil(s.su / cell)));
    s.nv = std::max(1, int(std::ceil(s.sv / cell)));
    s.cells.assign(size_t(s.nu) * size_t(s.nv), {});
    s.valid.assign(s.cells.size(), 1);
    if (s.kind == "floor")
      for (int j = 0; j < s.nv; ++j)
        for (int i = 0; i < s.nu; ++i) {
          Vec3 c = s.center(i, j, cell);
          for (auto& b : room.boxes)
            if (c.x() >= b.x0 && c.x() <= b.x1 && c.z() >= b.z0 && c.z() <= b.z1) s.valid[size_t(j) * size_t(s.nu) + size_t(i)] = 0;
        }
    surfs.push_back(std::move(s));
  }

  bool los(const Vec3& a, const Vec3& b) const {
    Vec3 d = b - a;
    Vec3 end = a + d * 0.995;  // the target surface itself must not block
    for (auto& bx : room.boxes)
      if (seg_box(a, end, bx)) return false;
    const auto& P = room.poly;
    for (size_t i = 0; i < P.size(); ++i) {
      auto [x0, z0] = P[i];
      auto [x1, z1] = P[(i + 1) % P.size()];
      double t = seg_hit(a.x(), a.z(), end.x(), end.z(), x0, z0, x1, z1);
      if (t > 0.0005) return false;
    }
    return true;
  }

  bool walkable(double x, double z, double margin) const {
    if (!in_poly(room.poly, x, z)) return false;
    const auto& P = room.poly;
    for (size_t i = 0; i < P.size(); ++i) {
      auto [x0, z0] = P[i];
      auto [x1, z1] = P[(i + 1) % P.size()];
      if (dist_seg(x, z, x0, z0, x1, z1) < margin) return false;
    }
    for (auto& b : room.boxes) {  // a person is a disc: Euclidean distance to the footprint, not a square margin
      double dx = std::max({b.x0 - x, 0.0, x - b.x1}), dz = std::max({b.z0 - z, 0.0, z - b.z1});
      if (std::hypot(dx, dz) < margin) return false;
    }
    return true;
  }

  size_t total_valid() const {
    size_t n = 0;
    for (auto& s : surfs)
      for (size_t k = 0; k < s.cells.size(); ++k) n += s.valid[k];
    return n;
  }
};

geometry::Intrinsics make_K() {
  geometry::Intrinsics K;
  K.width = 640, K.height = 480, K.fx = K.fy = 500, K.cx = 319.5, K.cy = 239.5;
  return K;
}

double gt_completeness(const World& w, const GuideParams& gp, size_t* sufficient_out = nullptr) {
  size_t ok = 0, tot = 0;
  for (auto& s : w.surfs)
    for (size_t k = 0; k < s.cells.size(); ++k) {
      if (!s.valid[k]) continue;
      ++tot;
      ok += s.cells[k].sufficient(gp);
    }
  if (sufficient_out) *sufficient_out = ok;
  return tot ? double(ok) / double(tot) : 0.0;
}

// ---- Human model -------------------------------------------------------------------------------
struct Goal {
  InstrType type = InstrType::Finish;
  double yaw_target = 0, pitch_target = 0, dist_left = 0, heading_target = 0, until_s = 0;
  bool turning = false, backwards = false, active = false;
};

struct Walker {
  double x, z, yaw, pitch = 0;
  double speed_scale = 1;
  double slow_until = -1, frozen_until = -1;
  bool done = false;
  Goal goal;
  struct Pending {
    Instruction ins;
    double due;
  };
  std::deque<Pending> pending;
  double path = 0, yaw_rate = 0, pitch_rate = 0, speed = 0;
  double level_at = -1;  // Human::hold_level: relax pitch to level from this time (< 0: not pending)
  int blocked = 0, received = 0, followed = 0;
};

// ---- Phase 9 step 7: gap truth (the sim's own ground truth, never fed back to the engine) -----------------------
struct Frac {
  int tot = 0, seen = 0, suff = 0;
  double seen_f() const { return tot ? double(seen) / tot : 0.0; }
  double suff_f() const { return tot ? double(suff) / tot : 0.0; }
};

/// Ground-truth cells of the chosen surface kinds whose centre falls in the rectangle (floor cells under blocks skipped).
Frac region(const World& w, const GuideParams& gp, double x0, double z0, double x1, double z1, bool walls, bool floor, bool ceil) {
  Frac f;
  for (const Surf& s : w.surfs) {
    if (!((s.kind == "wall" && walls) || (s.kind == "floor" && floor) || (s.kind == "ceiling" && ceil))) continue;
    for (int j = 0; j < s.nv; ++j)
      for (int i = 0; i < s.nu; ++i) {
        const size_t k = size_t(j) * size_t(s.nu) + size_t(i);
        if (!s.valid[k]) continue;
        const Vec3 c = s.center(i, j, w.cell);
        if (c.x() < x0 || c.x() > x1 || c.z() < z0 || c.z() > z1) continue;
        if (s.kind == "floor")
          if (std::any_of(w.room.boxes.begin(), w.room.boxes.end(), [&](const Box& b) { return c.x() >= b.x0 && c.x() <= b.x1 && c.z() >= b.z0 && c.z() <= b.z1; })) continue;
        ++f.tot, f.seen += s.cells[k].seen, f.suff += s.cells[k].sufficient(gp);
      }
  }
  return f;
}

Frac wall_frac(const Surf& s, const GuideParams& gp) {
  Frac f;
  for (size_t k = 0; k < s.cells.size(); ++k)
    if (s.valid[k]) ++f.tot, f.seen += s.cells[k].seen, f.suff += s.cells[k].sufficient(gp);
  return f;
}

double rect_gap(double ax0, double az0, double ax1, double az1, double bx0, double bz0, double bx1, double bz1) {
  const double dx = std::max({0.0, ax0 - bx1, bx0 - ax1}), dz = std::max({0.0, az0 - bz1, bz0 - az1});
  return std::hypot(dx, dz);
}

struct GapEval {
  struct T { int n = 0, hit = 0; };
  struct G { int n = 0, fals = 0, far = 0; };  // far: MissingWall farther than 1 m from any true wall
  std::map<std::string, T> truth;  // wall corner block ceiling floor
  std::map<std::string, G> gaps;   // by gap type name
  int checkpoints = 0;
};

constexpr double kGapMatchM = 0.8;      // gap centre within this of a true hidden wall / corner
constexpr double kHiddenSeen = 0.10;    // a true surface with at most this seen fraction is hidden
constexpr double kBlockHidden = 0.30;   // floor ring around a block seen below this = hidden behind the block
constexpr double kFalseSeen = 0.50;     // a gap whose true region was seen this much is a false gap
constexpr double kFalseSuff = 0.70;     // ... Weak: sufficient this much

void eval_gaps(const World& w, const GuideParams& gp, const std::vector<Gap>& gaps, GapEval& ev) {
  const auto& P = w.room.poly;
  const size_t nw = P.size();
  ++ev.checkpoints;
  auto cen = [](const Gap& g) { return std::pair<double, double>{0.5 * (g.x0 + g.x1), 0.5 * (g.z0 + g.z1)}; };
  std::vector<bool> hidden_wall(nw, false);
  for (size_t i = 0; i < nw; ++i) hidden_wall[i] = wall_frac(w.surfs[i], gp).seen_f() <= kHiddenSeen;
  // Truth: hidden walls.
  for (size_t i = 0; i < nw; ++i) {
    if (!hidden_wall[i]) continue;
    auto& t = ev.truth["wall"];
    ++t.n;
    auto [ax, az] = P[i];
    auto [bx, bz] = P[(i + 1) % nw];
    for (const Gap& g : gaps)
      if (g.type == GapType::MissingWall && dist_seg(cen(g).first, cen(g).second, ax, az, bx, bz) <= kGapMatchM) { ++t.hit; break; }
  }
  // Truth: hidden corners (neither adjacent wall is hidden on its own).
  for (size_t i = 0; i < nw; ++i) {
    if (hidden_wall[i] || hidden_wall[(i + nw - 1) % nw]) continue;
    auto [px, pz] = P[i];
    const Frac f = region(w, gp, px - 0.8, pz - 0.8, px + 0.8, pz + 0.8, true, false, false);
    if (f.tot == 0 || f.seen_f() > kHiddenSeen) continue;
    auto& t = ev.truth["corner"];
    ++t.n;
    for (const Gap& g : gaps)
      if (g.type == GapType::UnseenCorner && std::hypot(cen(g).first - px, cen(g).second - pz) <= kGapMatchM) { ++t.hit; break; }
  }
  // Truth: floor around a classed block never seen (behind / beside it).
  for (const Box& b : w.room.boxes) {
    if (!b.cls) continue;
    const double m = 0.6;
    const Frac outer = region(w, gp, b.x0 - m, b.z0 - m, b.x1 + m, b.z1 + m, false, true, false);
    if (outer.tot < 10 || outer.seen_f() >= kBlockHidden) continue;
    auto& t = ev.truth["block"];
    ++t.n;
    for (const Gap& g : gaps)
      if (g.type == GapType::Occluded && rect_gap(g.x0, g.z0, g.x1, g.z1, b.x0, b.z0, b.x1, b.z1) <= 0.6) { ++t.hit; break; }
  }
  // Truth: ceiling / floor never seen.
  for (int c = 0; c < 2; ++c) {
    const Frac f = region(w, gp, -1e9, -1e9, 1e9, 1e9, false, c == 0, c == 1);
    if (f.tot == 0 || f.seen_f() > kHiddenSeen) continue;
    auto& t = ev.truth[c ? "ceiling" : "floor"];
    ++t.n;
    for (const Gap& g : gaps)
      if (g.type == (c ? GapType::UnseenCeiling : GapType::UnseenFloor)) { ++t.hit; break; }
  }
  // False gaps: the true region was in fact well seen (or nothing is there).
  static const char* names[] = {"missing_wall", "unseen_corner", "unseen_ceiling", "unseen_floor", "occluded", "weak"};
  for (const Gap& g : gaps) {
    auto& e = ev.gaps[names[int(g.type)]];
    ++e.n;
    const auto [cx, cz] = cen(g);
    bool fals = false;
    switch (g.type) {
      case GapType::MissingWall: {
        double best = 1e18;
        size_t bi = 0;
        for (size_t i = 0; i < nw; ++i) {
          const double d = dist_seg(cx, cz, P[i].first, P[i].second, P[(i + 1) % nw].first, P[(i + 1) % nw].second);
          if (d < best) best = d, bi = i;
        }
        e.far += best > 1.0;
        fals = best > 1.0 || wall_frac(w.surfs[bi], gp).seen_f() >= kFalseSeen;
        break;
      }
      case GapType::UnseenCorner: {
        const Frac f = region(w, gp, cx - 0.8, cz - 0.8, cx + 0.8, cz + 0.8, true, false, false);
        fals = f.tot == 0 || f.seen_f() >= kFalseSeen;
        break;
      }
      case GapType::UnseenCeiling:
      case GapType::UnseenFloor: {
        const bool ceil = g.type == GapType::UnseenCeiling;
        fals = region(w, gp, -1e9, -1e9, 1e9, 1e9, false, !ceil, ceil).seen_f() >= kFalseSeen;
        break;
      }
      case GapType::Occluded: {
        const Box* nb = nullptr;
        double best = 1.5;
        for (const Box& b : w.room.boxes) {
          const double d = rect_gap(g.x0, g.z0, g.x1, g.z1, b.x0, b.z0, b.x1, b.z1);
          if (d < best) best = d, nb = &b;
        }
        fals = !nb || region(w, gp, nb->x0 - 0.6, nb->z0 - 0.6, nb->x1 + 0.6, nb->z1 + 0.6, false, true, false).seen_f() >= kFalseSeen;
        break;
      }
      case GapType::Weak:
        fals = region(w, gp, std::min(g.x0, g.x1) - 0.3, std::min(g.z0, g.z1) - 0.3, std::max(g.x0, g.x1) + 0.3, std::max(g.z0, g.z1) + 0.3, true, true, true).suff_f() >= kFalseSuff;
        break;
    }
    e.fals += fals;
  }
}

void turn_toward(double& cur, double target, double rate, double dt, double& out_rate) {
  double e = wrap_pi(target - cur);
  double step = std::clamp(e, -rate * dt, rate * dt);
  cur = wrap_pi(cur + step);
  out_rate = step / dt;
}

}  // namespace

std::string simulate(const Config& cfg) {
  const Room room = make_room(cfg.room);
  const GuideParams& gp = cfg.guide;
  World w(room, gp.cell_m);
  Rng rng(cfg.seed);
  // ---- "arcore" plane mode (ADR 0029): wall planes like ARCore emits them. Own RNG (prng): the walker, sensor and
  // engine draws stay where they were. Tracks are fixed at start: one per along-wall fragment between furniture pieces
  // standing against the wall (< 0.3 m off it, >= 0.3 m wide); a fragment > 2 m is first two half tracks (p 0.5)
  // that merge once both were emitted 8 times (the lower one takes the union, the other id goes stale = subsumed).
  const std::string pmode = cfg.planes.empty() ? (room.arcore_planes ? "arcore" : "ideal") : cfg.planes;
  const bool arcore = pmode == "arcore";
  Rng prng(cfg.seed * 104729 + 31);
  struct Track {
    int wall = 0;
    double a = 0, b = 0;  // along-wall interval (m from the wall's start)
    int partner = -1;     // pre-merge half: index of the other half
    std::string id;       // ARCore plane id; "" until first detected
    double yaw_err = 0, off_err = 0;
    int emits = 0;        // emissions under the current id
    bool alive = true;
  };
  std::vector<Track> tracks;
  std::vector<int> frag_by_wall(room.poly.size(), 0);
  int n_frag = 0, n_reid = 0, n_subsumed = 0, next_pid = 0, max_live = 0;
  double jit_min = 1e9, jit_max = 0;
  auto new_id = [&](Track& tr) {  // fresh id + fresh 2-4 deg yaw jitter and +-2 cm offset
    tr.id = "a" + std::to_string(next_pid++);
    const double mag = prng.uniform(2.0, 4.0);
    tr.yaw_err = prng.uniform() < 0.5 ? -mag : mag;
    tr.off_err = prng.uniform(-0.02, 0.02);
    tr.emits = 0;
    jit_min = std::min(jit_min, mag), jit_max = std::max(jit_max, mag);
  };
  // textureless wall: keeps 35 % of its points (prng; the point draws themselves are unchanged)
  auto thin = [&](int surf) { return arcore && surf >= 0 && surf == room.textureless_wall && prng.uniform() >= 0.35; };
  if (arcore)
    for (size_t i = 0; i < room.poly.size(); ++i) {
      Surf& s = w.surfs[i];  // walls are surfs[0..poly.size()) in edge order
      s.bin_n.assign(size_t(std::ceil(s.su / Surf::kBin)), 0);
      s.bin_vmin.assign(s.bin_n.size(), 1e9), s.bin_vmax.assign(s.bin_n.size(), -1e9);
      if (int(i) == room.textureless_wall) continue;
      std::vector<std::pair<double, double>> occ;  // along-wall intervals hidden by furniture against this wall
      for (const Box& b : room.boxes) {
        double dmin = 1e9, ua = 1e9, ub = -1e9;
        for (double x : {b.x0, b.x1})
          for (double z : {b.z0, b.z1}) {
            const Vec3 q = Vec3(x, 0, z) - s.o;
            dmin = std::min(dmin, q.dot(s.n)), ua = std::min(ua, q.dot(s.u)), ub = std::max(ub, q.dot(s.u));
          }
        ua = std::max(ua, 0.0), ub = std::min(ub, s.su);
        if (dmin > -0.05 && dmin < 0.3 && ub - ua >= 0.3) occ.push_back({ua, ub});
      }
      std::sort(occ.begin(), occ.end());
      auto add_frag = [&](double a, double b) {
        if (b - a < 0.3) return;
        ++frag_by_wall[i], ++n_frag;
        const int k = int(tracks.size());
        if (b - a > 2.0 && prng.uniform() < 0.5) {
          const double m = a + (b - a) * prng.uniform(0.35, 0.65);
          tracks.push_back({int(i), a, m, k + 1}), tracks.push_back({int(i), m, b, k});
        } else {
          tracks.push_back({int(i), a, b});
        }
      };
      double cur = 0;
      for (auto [oa, ob] : occ) add_frag(cur, oa), cur = std::max(cur, ob);
      add_frag(cur, s.su);
    }
  const double dt = 1.0 / 30, cam_h = 1.4;
  const geometry::Intrinsics K = make_K();
  const double exposure_s = 0.010;  // well-lit room; blur px = (rotation + translation/2 m) * exposure * fx
  quality::QualityConfig qc;
  quality::KeyframeSelector selector(qc);
  GuideEngine engine(gp);
  const bool use_engine = cfg.policy == "planner";
  // Detector oracle (Phase 9 step 3): own RNG, never feeds the engine: guidance output is unchanged. Any policy. The lifter
  // sees the TRUE shell planes (isolates lifting from plane detection, which steps 1-2 test).
  const bool oracle = cfg.detector_oracle;
  ObjectLifter lifter;
  SparsePointStore lift_pts;
  Rng orng(cfg.seed * 7919 + 17);
  int or_dets = 0, or_false = 0;
  // Gap harness (Phase 9 step 7): shadow stages only the report reads; nothing here feeds the engine or the walker.
  const bool ueval = cfg.understanding_eval && use_engine;
  RoomShell sshell;
  GapFinder gfinder;
  Coverage vcov(gp);
  GapEval geval;
  std::vector<Gap> last_gaps;

  Walker me{room.start_x, room.start_z, room.start_yaw};
  const double dtr = deg2rad(1.0);
  const Human& H = cfg.human;

  // Scripted policy: perimeter loop at ~1 m inset facing the wall, then centre spins.
  struct Way {
    double x, z;
  };
  std::vector<Way> script;
  size_t way_i = 0;
  double script_spin = 0;
  int spin_phase = 0;
  if (cfg.policy == "scripted") {
    // inset the rectilinear footprint by moving each vertex along the bisector toward the interior
    auto& P = room.poly;
    for (size_t i = 0; i < P.size(); ++i) {
      auto [x, z] = P[i];
      auto [xp, zp] = P[(i + P.size() - 1) % P.size()];
      auto [xn, zn] = P[(i + 1) % P.size()];
      double ax = x - xp, az = z - zp, bx = xn - x, bz = zn - z;
      double la = std::hypot(ax, az), lb = std::hypot(bx, bz);
      double n1x = -az / la, n1z = ax / la, n2x = -bz / lb, n2z = bx / lb;  // inward normals
      double sx = n1x + n2x, sz = n1z + n2z;
      double ls = std::hypot(sx, sz);
      double cosh = ls / 2;  // cos(half corner angle)
      double d = 1.3 / std::max(cosh, 0.3);  // a decent surveyor walks about 1.3 m off the wall
      script.push_back({x + sx / ls * d, z + sz / ls * d});
    }
    script.push_back(script.front());
    const size_t lap = script.size();  // two laps (the second lap with a different pitch phase)
    for (size_t i = 0; i < lap; ++i) script.push_back(script[i]);
  }
  double wander_t = 0, wander_turn = 0;

  // Per-frame stash for delayed keyframe decisions.
  struct Stash {
    geometry::PoseWC pose;
    double sharp;
    int64_t t;
  };
  // Furniture top faces seen so far (extent of visible points), for the ARCore-like "floor"-labelled table-top planes.
  struct Top {
    double xmin = 1e9, xmax = -1e9, zmin = 1e9, zmax = -1e9;
    int npts = 0;
  };
  std::vector<Top> tops(room.boxes.size());
  std::map<uint32_t, Stash> stash;
  uint32_t frame_id = 0;
  std::vector<quality::KeyframeSelector::Decision> decisions;

  double t80 = -1, t90 = -1, t95 = -1;
  int n_frames = 0, n_keyframes = 0, unsafe_gt = 0, last_target_id = 0;
  std::vector<std::pair<double, double>> series;
  std::vector<std::string> trace, overlay_samples;
  size_t overlay_max_bytes = 0;
  int tracking = 1, tracking_loss_events = 0;
  double lost_until = -1;
  double finish_issued_t = -1;
  int64_t t_ns = 0;
  const size_t total_cells = w.total_valid();
  std::vector<Instruction> new_ins;
  std::map<std::string, int> ins_seen;
  std::vector<std::pair<Vec3, double>> unused;
  (void)unused;
  const int steps = int(cfg.max_s / dt);
  double prev_yaw = me.yaw, prev_pitch = me.pitch;
  Vec3 prev_pos(me.x, cam_h, me.z);
  int step = 0;

  for (; step < steps && !me.done; ++step) {
    const double t = step * dt;
    t_ns = int64_t(step) * 33'333'333;

    // ---- sensors -> engine -------------------------------------------------------------------
    Vec3 pos(me.x + rng.normal() * 0.002, cam_h + 0.01 * std::sin(t * 2 * kPi * 1.5) * std::min(1.0, me.speed), me.z + rng.normal() * 0.002);
    double yaw_s = me.yaw + rng.normal() * dtr * 0.1, pitch_s = me.pitch + rng.normal() * dtr * 0.1;
    geometry::PoseWC T = pose_from_yaw_pitch(pos, yaw_s, pitch_s);
    double omega = std::hypot(me.yaw_rate, me.pitch_rate);
    if (lost_until >= 0 && t >= lost_until) lost_until = -1;
    int state = lost_until >= 0 ? 3 : (omega > deg2rad(110) ? 2 : 1);
    if (omega > deg2rad(160) && lost_until < 0) {
      lost_until = t + 1.5;
      state = 3;
    }
    if (state == 3 && tracking != 3) ++tracking_loss_events;
    tracking = state;

    if (use_engine) {
      bundle::PoseSample ps;
      ps.t_ns = t_ns;
      T.to_tq(ps.t_world_from_camera, ps.q_world_from_camera_wxyz);
      ps.tracking_state = bundle::TrackingState(state);
      if (step == 0) {
        bundle::IntrinsicsSample k;
        k.t_ns = t_ns, k.width = K.width, k.height = K.height, k.fx = k.fy = K.fx, k.cx = K.cx, k.cy = K.cy;
        engine.on_intrinsics(k);
      }
      engine.on_pose(ps);
    }

    ViewCam vc = ViewCam::make(T, K);
    const double cos_max = std::cos(deg2rad(gp.max_incidence_deg));

    // points (5 Hz) from visible surfaces and furniture faces
    if (step % 6 == 0 && state != 3) {
      bundle::PointCloudRecord pc;
      pc.t_ns = t_ns;
      if (cfg.view_points) {  // ADR 0015: points come from what this frame views (ray casts through a pixel grid)
        const Eigen::Matrix3d R_wc = vc.R_cw.transpose();
        for (int gy = 0; gy < 10; ++gy)
          for (int gx = 0; gx < 12; ++gx) {
            const double px = (gx + rng.uniform()) * K.width / 12.0, py = (gy + rng.uniform()) * K.height / 10.0;
            const Vec3 dir = (R_wc * Vec3((px - K.cx) / K.fx, (py - K.cy) / K.fy, 1.0)).normalized();
            const World::Hit hit = w.raycast(vc.pos, dir);
            if (!hit.ok) continue;
            const double range = hit.t, cos_inc = std::fabs(hit.n.dot(dir));
            const double keep = std::clamp((5.0 - range) / (5.0 - 1.5), 0.0, 1.0);
            if (range < gp.range_min_m || cos_inc < 0.1 || rng.uniform() >= keep) continue;
            const double sigma = 0.01 + 0.005 * std::max(0.0, range - 2.0);
            const double nx = rng.normal(), ny = rng.normal(), nz = rng.normal();  // fixed draw order
            const Vec3 q = hit.p + Vec3(nx, ny, nz) * sigma;
            if (thin(hit.surf)) continue;
            pc.xyz.push_back(float(q.x())), pc.xyz.push_back(float(q.y())), pc.xyz.push_back(float(q.z()));
            if (hit.surf >= 0) {
              Surf& sf = w.surfs[size_t(hit.surf)];
              sf.note((hit.p - sf.o).dot(sf.u), (hit.p - sf.o).dot(sf.v));
            } else if (hit.face >= 0 && hit.n.y() > 0.5) {  // a furniture top face
              Top& tp = tops[size_t(hit.face) / 5];
              tp.xmin = std::min(tp.xmin, hit.p.x()), tp.xmax = std::max(tp.xmax, hit.p.x());
              tp.zmin = std::min(tp.zmin, hit.p.z()), tp.zmax = std::max(tp.zmax, hit.p.z());
              ++tp.npts;
            }
          }
      } else {
      double total_w = 0;
      for (double x : w.weights) total_w += x;
      for (int tries = 0; tries < 160 && pc.xyz.size() < 3 * 40; ++tries) {
        double r = rng.uniform() * total_w, acc = 0;
        size_t idx = 0;
        for (; idx < w.weights.size(); ++idx) {
          acc += w.weights[idx];
          if (r <= acc) break;
        }
        idx = std::min(idx, w.weights.size() - 1);
        Vec3 p, n;
        Surf* sf = nullptr;
        double a = rng.uniform(), b = rng.uniform();
        if (idx < w.surfs.size()) {
          sf = &w.surfs[idx];
          int i = int(a * sf->nu), j = int(b * sf->nv);
          if (!sf->valid[size_t(j) * size_t(sf->nu) + size_t(i)]) continue;
          p = sf->center(i, j, w.cell);
          n = sf->n;
        } else if (idx < w.surfs.size() + w.faces.size()) {
          const Face& f = w.faces[idx - w.surfs.size()];
          p = f.o + f.u * (a * f.su) + f.v * (b * f.sv);
          n = f.n;
        } else {  // seen through a door / window (only rooms with openings have these)
          const size_t k = idx - w.surfs.size() - w.faces.size();
          const Face& f = w.through[k];
          p = f.o + f.u * (a * f.su) + f.v * (b * f.sv);
          VisResult vr = view_cell(vc, p, f.n, gp.view_margin_px);
          if (!vr.in_image || vr.range > 6.0 || vr.cos_inc < 0.1 || !w.through_visible(vc.pos, p, k)) continue;
          Vec3 q = p + Vec3(rng.normal(), rng.normal(), rng.normal()) * 0.01;
          pc.xyz.push_back(float(q.x())), pc.xyz.push_back(float(q.y())), pc.xyz.push_back(float(q.z()));
          continue;
        }
        VisResult vr = view_cell(vc, p, n, gp.view_margin_px);
        if (!vr.in_image || vr.range > 6.0 || vr.cos_inc < 0.1 || !w.los(vc.pos, p)) continue;
        Vec3 q = p + Vec3(rng.normal(), rng.normal(), rng.normal()) * 0.01;
        if (sf && thin(int(idx))) continue;
        pc.xyz.push_back(float(q.x())), pc.xyz.push_back(float(q.y())), pc.xyz.push_back(float(q.z()));
        if (!sf && n.y() > 0.5) {  // a furniture top face (no RNG use: reports with the flag off are unchanged)
          Top& tp = tops[(idx - w.surfs.size()) / 5];
          tp.xmin = std::min(tp.xmin, p.x()), tp.xmax = std::max(tp.xmax, p.x());
          tp.zmin = std::min(tp.zmin, p.z()), tp.zmax = std::max(tp.zmax, p.z());
          ++tp.npts;
        }
        if (sf) sf->note((p - sf->o).dot(sf->u), (p - sf->o).dot(sf->v));
      }
      }  // !cfg.view_points
      if (oracle)
        for (size_t i = 0; i + 2 < pc.xyz.size(); i += 3) lift_pts.add({pc.xyz[i], pc.xyz[i + 1], pc.xyz[i + 2]});
      if (use_engine && !pc.xyz.empty()) engine.on_points(pc);
    }
    // planes (1 Hz): rectangles that grow as more is seen
    if (use_engine && step % 30 == 15) {
      for (size_t i = 0; i < w.surfs.size(); ++i) {
        const Surf& s = w.surfs[i];
        if (s.npts < 15 || (arcore && i < room.poly.size())) continue;  // arcore mode: wall planes come from the tracks
        bundle::PlaneRecord pr;
        pr.t_ns = t_ns + int64_t(i);
        pr.id = std::to_string(i);
        pr.kind = s.kind;
        Vec3 c = s.o + s.u * (0.5 * (s.umin + s.umax)) + s.v * (0.5 * (s.vmin + s.vmax));
        for (int k = 0; k < 3; ++k) pr.center[k] = c[k], pr.normal[k] = s.n[k], pr.axis_u[k] = s.u[k];
        pr.extent_u = s.umax - s.umin;
        pr.extent_v = s.vmax - s.vmin;
        engine.on_plane(pr);
      }
      // Phase 9: ARCore labels every upward plane "floor", furniture tops included. Emit them like that so the
      // SurfaceClassifier has to tell them apart (only with understanding on: flag-off reports stay identical).
      if (gp.understanding != 0)
        for (size_t i = 0; i < tops.size(); ++i) {
          const Top& tp = tops[i];
          if (tp.npts < 15 || !(tp.xmax > tp.xmin) || !(tp.zmax > tp.zmin)) continue;
          bundle::PlaneRecord pr;
          pr.t_ns = t_ns + int64_t(w.surfs.size() + i);
          pr.id = "top" + std::to_string(i);
          pr.kind = "floor";
          pr.center[0] = 0.5 * (tp.xmin + tp.xmax), pr.center[1] = room.boxes[i].h, pr.center[2] = 0.5 * (tp.zmin + tp.zmax);
          pr.normal[1] = 1, pr.axis_u[0] = 1;
          pr.extent_u = tp.xmax - tp.xmin, pr.extent_v = tp.zmax - tp.zmin;
          engine.on_plane(pr);
        }
    }
    if (arcore && step % 30 == 15) {  // arcore wall planes (1 Hz, any policy: the stats are the realism check)
      int live = 0;
      int64_t k_emit = int64_t(w.surfs.size() + tops.size());
      for (size_t ti = 0; ti < tracks.size(); ++ti) {
        Track& tr = tracks[ti];
        if (!tr.alive) continue;
        if (tr.partner > int(ti)) {  // subsumption: both halves seen 8 times -> this one takes the union
          Track& o = tracks[size_t(tr.partner)];
          if (tr.emits >= 8 && o.emits >= 8) {
            tr.a = std::min(tr.a, o.a), tr.b = std::max(tr.b, o.b);
            o.alive = false, o.partner = tr.partner = -1;
            ++n_subsumed;
          }
        }
        const Surf& s = w.surfs[size_t(tr.wall)];
        int n = 0;
        double ua = 1e9, ub = -1e9, va = 1e9, vb = -1e9;
        for (size_t b = 0; b < s.bin_n.size(); ++b) {
          const double b0 = double(b) * Surf::kBin, b1 = b0 + Surf::kBin, bc = b0 + 0.5 * Surf::kBin;
          if (!s.bin_n[b] || bc < tr.a || bc > tr.b) continue;
          n += s.bin_n[b];
          ua = std::min(ua, std::max(b0, tr.a)), ub = std::max(ub, std::min(b1, tr.b));
          va = std::min(va, s.bin_vmin[b]), vb = std::max(vb, s.bin_vmax[b]);
        }
        if (n < 10 || ub - ua < 0.2 || vb - va < 0.2) continue;  // not detected (yet)
        if (tr.id.empty()) {
          new_id(tr);
        } else if (tr.emits >= 3 && prng.uniform() < 0.01) {  // re-id: same surface, new id; the old one goes stale
          new_id(tr);
          ++n_reid;
        }
        ++tr.emits, ++live;
        if (!use_engine) continue;
        const double cy = std::cos(deg2rad(tr.yaw_err)), sy = std::sin(deg2rad(tr.yaw_err));
        auto rot = [&](const Vec3& d) { return Vec3(cy * d.x() + sy * d.z(), d.y(), -sy * d.x() + cy * d.z()); };
        const Vec3 c = s.o + s.u * (0.5 * (ua + ub)) + s.v * (0.5 * (va + vb)) + s.n * tr.off_err;
        const Vec3 nn = rot(s.n), uu = rot(s.u);
        bundle::PlaneRecord pr;
        pr.t_ns = t_ns + k_emit++;
        pr.id = tr.id;
        pr.kind = "wall";
        for (int k = 0; k < 3; ++k) pr.center[k] = c[k], pr.normal[k] = nn[k], pr.axis_u[k] = uu[k];
        pr.extent_u = ub - ua;
        pr.extent_v = vb - va;
        engine.on_plane(pr);
      }
      max_live = std::max(max_live, live);
    }
    if (ueval && step % 30 == 15) {  // shadow shell + virtual surfaces follow the engine's map at 1 Hz
      sshell.update(engine.map(), nullptr, engine.openings());
      vcov.sync_virtual(GapFinder::virtual_surfaces(sshell.shell(), engine.map()));
    }
    // frames (10 Hz): quality from motion, keyframe selection with the real selector
    if (step % 3 == 0) {
      double blur = omega * exposure_s * K.fx + (me.speed / 2.0) * exposure_s * K.fx;
      quality::FrameMetrics m;
      m.blur_risk_px = blur;
      m.sharpness_norm = std::max(0.2, 1.0 - blur / 8.0);
      m.tracking_state = state;
      if (state == 3) m.hard_reject = true, m.reasons |= quality::kTrackingLost;
      else if (blur > qc.blur_reject_px) m.hard_reject = true, m.reasons |= quality::kMotionBlur;
      else if (blur > qc.blur_soft_px) m.reasons |= quality::kMotionBlur;
      m.quality = m.hard_reject ? 0.0 : std::clamp(1.0 - 0.1 * blur, 0.0, 1.0);
      uint32_t id = frame_id++;
      ++n_frames;
      stash[id] = {T, m.sharpness_norm, t_ns};
      decisions.clear();
      selector.push(id, t_ns, m, T, decisions);
      if (use_engine) {
        FrameObs fo;
        fo.t_ns = t_ns, fo.frame_id = id, fo.hard_reject = m.hard_reject, fo.motion_blur = (m.reasons & quality::kMotionBlur) != 0;
        engine.on_frame(fo);
      }
      for (auto& d : decisions) {
        auto it = stash.find(d.frame_id);
        if (it == stash.end()) continue;
        if (d.keyframe) {
          ++n_keyframes;
          if (use_engine) engine.on_keyframe(d.frame_id, it->second.t, it->second.sharp);
          if (ueval) {
            Obs vo;
            vo.pose = it->second.pose, vo.K = K, vo.t_ns = it->second.t, vo.sharp_norm = it->second.sharp, vo.keyframe = true;
            vcov.observe(vo, nullptr, nullptr);
          }
          if (oracle) {
            const geometry::PoseWC& Tk = it->second.pose;
            const ViewCam kc = ViewCam::make(Tk, K);
            const Eigen::Matrix3d Rwc = kc.R_cw.transpose();
            LiftContext lc;
            lc.T_world_from_camera = Tk, lc.K = K, lc.has_floor = true, lc.floor_y = 0.0;
            for (const Surf& sf : w.surfs) {
              PlaneGeom g;
              g.c = sf.o + sf.u * (0.5 * sf.su) + sf.v * (0.5 * sf.sv), g.n = sf.n;
              lc.surfaces.push_back(g);
            }
            bundle::DetectionsRecord erec;  // understanding >= 2: the engine gets the same boxes (flag 1 keeps the sim-only oracle)
            erec.t_ns = it->second.t, erec.frame_id = d.frame_id;
            auto feed = [&](ObjectClass cls, double x0, double y0, double x1, double y1, double score) {
              erec.boxes.push_back({int32_t(cls), float(score), float(x0), float(y0), float(x1), float(y1)});
              Detection dd;
              dd.t_ns = it->second.t, dd.frame_id = d.frame_id, dd.cls = cls, dd.score = float(score);
              dd.x0 = float(x0), dd.y0 = float(y0), dd.x1 = float(x1), dd.y1 = float(y1);
              lifter.on_detection(dd, lc, lift_pts);
            };
            for (size_t bi = 0; bi < room.boxes.size(); ++bi) {
              const Box& b = room.boxes[bi];
              if (!b.cls) continue;
              double umin = 1e9, umax = -1e9, vmin = 1e9, vmax = -1e9;
              bool front = true;
              for (int c = 0; c < 8 && front; ++c) {
                const Vec3 cp(c & 1 ? b.x1 : b.x0, c & 2 ? b.h : 0.0, c & 4 ? b.z1 : b.z0);
                const Vec3 q = kc.R_cw * (cp - kc.pos);
                if (q.z() < 0.1) front = false;
                else {
                  const double u = K.fx * q.x() / q.z() + K.cx, v = K.fy * q.y() / q.z() + K.cy;
                  umin = std::min(umin, u), umax = std::max(umax, u), vmin = std::min(vmin, v), vmax = std::max(vmax, v);
                }
              }
              if (!front) continue;
              const double u0 = std::max(0.0, umin), u1 = std::min(double(K.width), umax);
              const double v0 = std::max(0.0, vmin), v1 = std::min(double(K.height), vmax);
              if (u1 <= u0 || v1 <= v0) continue;
              const double area = (u1 - u0) * (v1 - v0);
              if (area < 400 || area < 0.25 * (umax - umin) * (vmax - vmin)) continue;
              int vis = 0;  // 5 x 5 rays through the clipped box: how much of it is really this box (occlusion)
              for (int gy = 0; gy < 5; ++gy)
                for (int gx = 0; gx < 5; ++gx) {
                  const double px = u0 + (gx + 0.5) / 5 * (u1 - u0), py = v0 + (gy + 0.5) / 5 * (v1 - v0);
                  const Vec3 dir = (Rwc * Vec3((px - K.cx) / K.fx, (py - K.cy) / K.fy, 1.0)).normalized();
                  const World::Hit h = w.raycast(kc.pos, dir);
                  vis += h.ok && h.face >= 0 && size_t(h.face) / 5 == bi && h.t <= 6.0;
                }
              if (vis < 8) continue;
              const double miss = orng.uniform(), n0 = orng.normal(), n1 = orng.normal(), n2 = orng.normal(), n3 = orng.normal();
              const double score = 0.55 + 0.4 * orng.uniform();
              if (miss < cfg.oracle_miss) continue;
              const double bw = u1 - u0, bh = v1 - v0;
              ++or_dets;
              feed(ObjectClass(b.cls), u0 + 0.03 * bw * n0, v0 + 0.03 * bh * n1, u1 + 0.03 * bw * n2, v1 + 0.03 * bh * n3, score);
            }
            if (orng.uniform() < cfg.oracle_false) {  // a detection with no object behind it
              static const ObjectClass fc[3] = {ObjectClass::Chair, ObjectClass::Couch, ObjectClass::PottedPlant};
              const ObjectClass cls = fc[orng.index(3)];
              const double fw = orng.uniform(60, 160), fh = orng.uniform(60, 160);
              const double fx0 = orng.uniform(0, K.width - fw), fy0 = orng.uniform(0, K.height - fh);
              ++or_false;
              feed(cls, fx0, fy0, fx0 + fw, fy0 + fh, orng.uniform(0.3, 0.6));
            }
            if (use_engine && gp.understanding >= 2) engine.on_detections(erec);
          }
          // ground truth with the same rule + true occlusion
          ViewCam gc = ViewCam::make(it->second.pose, K);
          for (auto& s : w.surfs)
            for (int j = 0; j < s.nv; ++j)
              for (int i = 0; i < s.nu; ++i) {
                size_t k = size_t(j) * size_t(s.nu) + size_t(i);
                if (!s.valid[k]) continue;
                Vec3 p = s.center(i, j, w.cell);
                VisResult vr = view_cell(gc, p, s.n, gp.view_margin_px);
                if (!vr.in_image || vr.range > gp.range_max_m * 1.5 || !w.los(gc.pos, p)) continue;
                bool q = vr.range >= gp.range_min_m && vr.range <= gp.range_max_m && vr.cos_inc >= cos_max && it->second.sharp >= gp.min_sharp_norm;
                if (q) s.cells[k].add_view(vr.ray, vr.range, it->second.sharp, it->second.t);
                else s.cells[k].see(it->second.t);
              }
        }
        stash.erase(it);
      }
    }
    // coverage milestones (1 Hz)
    if (step % 30 == 0) {
      double c = gt_completeness(w, gp);
      series.push_back({t, c});
      if (t80 < 0 && c >= 0.80) t80 = t;
      if (t90 < 0 && c >= 0.90) t90 = t;
      if (t95 < 0 && c >= 0.95) t95 = t;
      if (c >= 0.98 && cfg.policy != "planner") break;
    }
    if (ueval && step % std::max(1, int(std::lround(cfg.understanding_every_s * 30))) == 0 && t >= cfg.understanding_every_s) {
      const std::vector<Object> objs = oracle ? lifter.objects() : std::vector<Object>{};
      GapInputs gi;
      gi.shell = &sshell.shell(), gi.map = &engine.map(), gi.objects = &objs, gi.openings = &engine.openings();
      gi.cov = &engine.coverage(), gi.vcov = &vcov;
      gi.user.has_user = true, gi.user.ux = me.x, gi.user.uz = me.z, gi.user.yaw = me.yaw;
      last_gaps = gfinder.find(gi);
      eval_gaps(w, gp, last_gaps, geval);
    }

    // ---- policy -> motion --------------------------------------------------------------------
    double yr = 0, pr = 0, v_cmd = 0, backward = 0;
    if (use_engine) {
      new_ins.clear();
      engine.update(t_ns, new_ins);
      if (cfg.overlay_every_s > 0 && step % 15 == 0) {  // the session emits the overlay every 500 ms of event time
        std::string oj = engine.overlay(t_ns).to_json();
        overlay_max_bytes = std::max(overlay_max_bytes, oj.size());
        if (step % std::max(1, int(std::lround(cfg.overlay_every_s * 30))) == 0) overlay_samples.push_back(oj);
      }
      {  // ground-truth safety: a target must be walkable in the TRUE room (not just in the engine's map)
        const Target& tg = engine.planner().current();
        if (tg.valid && tg.id != last_target_id) {
          last_target_id = tg.id;
          if (!w.walkable(tg.x, tg.z, 0.25)) {
            ++unsafe_gt;
            if (cfg.trace) trace.push_back(json::str("UNSAFE target " + std::to_string(tg.x) + "," + std::to_string(tg.z) + " at t=" + std::to_string(int(t)) + " user " + std::to_string(me.x) + "," + std::to_string(me.z) + " fallback=" + std::to_string(tg.fallback)));
          }
        }
      }
      for (auto& ins : new_ins) {
        if (cfg.trace) trace.push_back(json::str(std::to_string(int(t)) + "s " + type_name(ins.type) + " " + ins.speak_text + " @(" + std::to_string(me.x).substr(0, 4) + "," + std::to_string(me.z).substr(0, 4) + ") yaw" + std::to_string(int(rad2deg(me.yaw))) + " tgt(" + std::to_string(engine.planner().current().x).substr(0, 4) + "," + std::to_string(engine.planner().current().z).substr(0, 4) + ") mode" + std::to_string(int(engine.last_plan().mode))));
        ++ins_seen[type_name(ins.type)];
        ++me.received;
        if (ins.type == InstrType::Finish && finish_issued_t < 0) finish_issued_t = t;
        if (rng.uniform() > H.compliance) continue;  // ignored
        ++me.followed;
        double due = t + rng.uniform(H.delay_min, H.delay_max);
        if (ins.priority == 3) {  // safety reactions are faster
          due = t + rng.uniform(0.3, 1.0);
          me.pending.clear();
        } else {
          me.pending.erase(std::remove_if(me.pending.begin(), me.pending.end(), [](auto& p) { return p.ins.priority <= 2; }), me.pending.end());
        }
        me.pending.push_back({ins, due});
      }
      // start the next due instruction (urgent ones interrupt)
      for (auto it = me.pending.begin(); it != me.pending.end();) {
        if (it->due > t) {
          ++it;
          continue;
        }
        if (me.goal.active && it->ins.priority < 3) {  // busy: keep waiting
          ++it;
          continue;
        }
        const Instruction& ins = it->ins;
        Goal g;
        g.type = ins.type;
        g.active = true;
        switch (ins.type) {
          case InstrType::RotateToward:
            g.yaw_target = wrap_pi(me.yaw + deg2rad(ins.param("rel_deg")) + rng.normal() * deg2rad(H.yaw_noise_deg));
            break;
          case InstrType::MoveTo:
            g.heading_target = wrap_pi(me.yaw + deg2rad(ins.param("rel_bearing_deg")) + rng.normal() * deg2rad(H.yaw_noise_deg));
            g.dist_left = ins.param("distance_m") * (1 + 0.1 * rng.normal());
            g.turning = true;
            break;
          case InstrType::TiltUp: g.pitch_target = std::min(deg2rad(45), me.pitch + deg2rad(ins.param("deg"))); break;
          case InstrType::TiltDown: g.pitch_target = std::max(deg2rad(-45), me.pitch - deg2rad(ins.param("deg"))); break;
          case InstrType::StepBack: g.dist_left = ins.param("meters", 0.5); g.backwards = true; break;
          case InstrType::Sweep: g.yaw_target = 0, g.dist_left = 2 * kPi; break;  // dist_left = angle left to turn
          case InstrType::SlowDown: me.slow_until = t + 5; g.active = false; break;
          case InstrType::HoldStill: case InstrType::Relocalize: me.frozen_until = t + (ins.type == InstrType::HoldStill ? 3.0 : 5.0); g.active = false; break;
          case InstrType::Finish: me.done = true; g.active = false; break;
          default: g.active = false; break;
        }
        if (g.active) me.goal = g;
        if (H.hold_level && g.active && (g.type == InstrType::TiltUp || g.type == InstrType::TiltDown)) me.level_at = -1;
        it = me.pending.erase(it);
      }
      if (t < me.frozen_until) me.goal.active = false;
    } else if (cfg.policy == "scripted") {
      if (way_i < script.size()) {
        const Way& wp = script[way_i];
        double dx = wp.x - me.x, dz = wp.z - me.z, d = std::hypot(dx, dz);
        if (d < 0.25 || me.blocked > 45) {
          ++way_i, me.blocked = 0;
        } else {
          // face the nearest wall (outward), sinusoidal pitch sweep, walk toward the waypoint
          double toward = std::atan2(dx, dz);
          double wall_yaw = toward + kPi / 2;  // CCW loop: the wall is on the left
          turn_toward(me.yaw, wall_yaw, deg2rad(H.turn_dps), dt, yr);
          me.pitch = deg2rad(H.hold_level ? 6 : 22) * std::sin(t * 0.35 + (way_i >= script.size() / 2 ? kPi : 0.0));
          pr = 0;
          v_cmd = H.speed * 0.8;
          // walk toward waypoint regardless of facing direction (side-stepping while looking at the wall)
          double nx = me.x + dx / d * v_cmd * dt, nz = me.z + dz / d * v_cmd * dt;
          if (w.walkable(nx, nz, 0.25)) me.x = nx, me.z = nz, me.blocked = 0;
          else ++me.blocked, v_cmd = 0;
        }
      } else {
        // centre spins at three pitches
        double rate = deg2rad(20);
        me.yaw = wrap_pi(me.yaw + rate * dt);
        yr = rate;
        script_spin += rate * dt;
        double target_p = spin_phase == 0 ? 0 : spin_phase == 1 ? deg2rad(30) : deg2rad(-30);
        turn_toward(me.pitch, target_p, deg2rad(H.pitch_dps), dt, pr);
        if (script_spin >= 2 * kPi) script_spin = 0, ++spin_phase;
        if (spin_phase >= 3) way_i = 0, spin_phase = 0, script_spin = 0;  // diligent surveyor: repeat the routine
      }
    } else {  // wander
      // straight for 3-6 s, then a short turn burst (random direction), turning away when blocked
      wander_t -= dt;
      if (wander_t <= 0) {
        if (wander_turn == 0) wander_turn = (rng.uniform() < 0.5 ? -1.0 : 1.0), wander_t = rng.uniform(0.6, 1.6);
        else wander_turn = 0, wander_t = rng.uniform(3, 6);
      }
      if (wander_turn != 0) me.yaw = wrap_pi(me.yaw + wander_turn * deg2rad(25) * dt), yr = wander_turn * deg2rad(25);
      me.pitch = deg2rad(H.hold_level ? 6 : 15) * std::sin(t * 0.3 + 1.0);
      v_cmd = H.speed * 0.7;
      double nx = me.x + std::sin(me.yaw) * v_cmd * dt, nz = me.z + std::cos(me.yaw) * v_cmd * dt;
      if (w.walkable(nx, nz, 0.25)) me.x = nx, me.z = nz;
      else if (wander_turn == 0) wander_turn = (rng.uniform() < 0.5 ? -1.0 : 1.0), wander_t = rng.uniform(2.0, 4.0), v_cmd = 0;
      else v_cmd = 0;
    }

    if (use_engine && me.goal.active && t >= me.frozen_until) {
      Goal& g = me.goal;
      double scale = t < me.slow_until ? 0.5 : 1.0;
      switch (g.type) {
        case InstrType::RotateToward:
          turn_toward(me.yaw, g.yaw_target, deg2rad(H.turn_dps) * scale, dt, yr);
          if (std::fabs(wrap_pi(g.yaw_target - me.yaw)) < deg2rad(2)) g.active = false;
          break;
        case InstrType::MoveTo: {
          if (g.turning) {
            turn_toward(me.yaw, g.heading_target, deg2rad(H.turn_dps) * scale, dt, yr);
            if (std::fabs(wrap_pi(g.heading_target - me.yaw)) < deg2rad(8)) g.turning = false;
          } else {
            double step_d = H.speed * scale * dt;
            double nx = me.x + std::sin(me.yaw) * step_d, nz = me.z + std::cos(me.yaw) * step_d;
            if (w.walkable(nx, nz, 0.25)) me.x = nx, me.z = nz, g.dist_left -= step_d, v_cmd = H.speed * scale;
            else ++me.blocked, g.active = false;
            if (g.dist_left <= 0) g.active = false;
          }
          break;
        }
        case InstrType::TiltUp: case InstrType::TiltDown:
          turn_toward(me.pitch, g.pitch_target, deg2rad(H.pitch_dps), dt, pr);
          if (std::fabs(g.pitch_target - me.pitch) < deg2rad(2)) {
            g.active = false;
            if (H.hold_level) me.level_at = t + gp.dwell_s + 0.5;  // hold the look past the planner dwell, then back to level
          }
          break;
        case InstrType::StepBack: {
          double step_d = 0.5 * dt;
          double nx = me.x - std::sin(me.yaw) * step_d, nz = me.z - std::cos(me.yaw) * step_d;
          if (w.walkable(nx, nz, 0.25)) me.x = nx, me.z = nz, g.dist_left -= step_d, backward = 0.5;
          else ++me.blocked, g.active = false;
          if (g.dist_left <= 0) g.active = false;
          break;
        }
        case InstrType::Sweep: {
          double rate = deg2rad(20) * scale;  // 'turn slowly': faster would blur every frame
          me.yaw = wrap_pi(me.yaw + rate * dt);
          yr = rate;
          g.dist_left -= rate * dt;
          if (g.dist_left <= 0) g.active = false;
          break;
        }
        default: g.active = false; break;
      }
    }

    // hold_level: relax pitch toward level after a completed tilt (no RNG draw; off = unchanged code path)
    if (H.hold_level && me.level_at >= 0 && t >= me.level_at && t >= me.frozen_until &&
        !(me.goal.active && (me.goal.type == InstrType::TiltUp || me.goal.type == InstrType::TiltDown))) {
      turn_toward(me.pitch, 0.0, deg2rad(H.pitch_dps), dt, pr);
      if (std::fabs(me.pitch) < deg2rad(1)) me.level_at = -1;
    }

    // bookkeeping for the next step
    me.yaw_rate = wrap_pi(me.yaw - prev_yaw) / dt;
    me.pitch_rate = (me.pitch - prev_pitch) / dt;
    Vec3 now(me.x, cam_h, me.z);
    me.speed = std::hypot(now.x() - prev_pos.x(), now.z() - prev_pos.z()) / dt;
    me.path += std::hypot(now.x() - prev_pos.x(), now.z() - prev_pos.z());
    prev_yaw = me.yaw, prev_pitch = me.pitch, prev_pos = now;
    (void)v_cmd; (void)backward; (void)yr; (void)pr;
    if (finish_issued_t >= 0 && me.done) break;
  }

  // ---- report -----------------------------------------------------------------------------------
  const double dur = step * dt;
  size_t suff = 0;
  double gt_final = gt_completeness(w, gp, &suff);
  if (t80 < 0 && gt_final >= 0.80) t80 = dur;
  if (t90 < 0 && gt_final >= 0.90) t90 = dur;
  if (t95 < 0 && gt_final >= 0.95) t95 = dur;
  auto r3 = [](double x) { return std::round(x * 1000) / 1000; };
  json::Object o;
  o.set("room", cfg.room).set("policy", cfg.policy).set("seed", int64_t(cfg.seed)).set("params_hash", gp.hash());
  o.set("duration_s", r3(dur)).set("path_m", r3(me.path)).set("finished", me.done);
  {
    struct Acc { size_t tot = 0, suff = 0, viewed = 0, seen = 0; };
    std::map<std::string, Acc> by;
    for (auto& sf : w.surfs)
      for (size_t k = 0; k < sf.cells.size(); ++k) {
        if (!sf.valid[k]) continue;
        auto& e = by[sf.kind];
        ++e.tot;
        e.suff += sf.cells[k].sufficient(gp);
        e.viewed += sf.cells[k].views > 0;
        e.seen += sf.cells[k].seen;
      }
    json::Object kb;
    for (auto& [k, v] : by) {
      json::Object d;
      d.set("sufficient", r3(double(v.suff) / double(v.tot))).set("viewed", r3(double(v.viewed) / double(v.tot))).set("seen", r3(double(v.seen) / double(v.tot)));
      kb.raw(k, d.dump());
    }
    o.raw("coverage_gt_by_kind", kb.dump());
  }
  o.set("coverage_gt_final", r3(gt_final)).set("cells", uint64_t(total_cells));
  o.set("t80_s", t80 < 0 ? -1.0 : r3(t80)).set("t90_s", t90 < 0 ? -1.0 : r3(t90)).set("t95_s", t95 < 0 ? -1.0 : r3(t95));
  {
    size_t seen = 0, viewed = 0, maxv = 0;
    for (auto& sf : w.surfs)
      for (size_t k = 0; k < sf.cells.size(); ++k) {
        seen += sf.cells[k].seen, viewed += sf.cells[k].views > 0, maxv = std::max<size_t>(maxv, sf.cells[k].views);
      }
    o.set("dbg_seen", uint64_t(seen)).set("dbg_viewed", uint64_t(viewed)).set("dbg_maxviews", uint64_t(maxv));
  }
  o.set("frames", n_frames).set("keyframes", n_keyframes).set("end_x", r3(me.x)).set("end_z", r3(me.z));
  o.set("tracking_loss_events", tracking_loss_events).set("blocked_events", me.blocked);
  std::vector<std::string> ser;
  for (size_t i = 0; i < series.size(); i += 10)
    ser.push_back("[" + json::num(r3(series[i].first)) + "," + json::num(r3(series[i].second)) + "]");
  o.raw("coverage_series_10s", json::array(ser.begin(), ser.end()));
  if (cfg.trace) o.raw("trace", json::array(trace.begin(), trace.end()));
  if (arcore) {  // ADR 0029: what the plane model did (absent in "ideal" mode: those reports are unchanged)
    json::Object sp;
    std::vector<std::string> fb;
    for (int f : frag_by_wall) fb.push_back(std::to_string(f));
    sp.set("mode", pmode).set("true_walls", uint64_t(room.poly.size())).set("textureless_walls", room.textureless_wall >= 0 ? 1 : 0);
    sp.set("fragments", n_frag).raw("fragments_by_wall", json::array(fb.begin(), fb.end())).set("tracks", uint64_t(tracks.size()));
    sp.set("wall_plane_ids", next_pid).set("reids", n_reid).set("subsumed", n_subsumed).set("max_live_wall_planes", max_live);
    sp.set("jitter_deg_min", next_pid ? r3(jit_min) : 0.0).set("jitter_deg_max", r3(jit_max));
    o.raw("sim_planes", sp.dump());
  }
  if (use_engine) {
    auto st = engine.coverage().stats();
    o.set("coverage_claimed_final", r3(st.completeness())).set("tier", std::string(1, engine.tier()));
    o.set("finish_issued_s", finish_issued_t < 0 ? -1.0 : r3(finish_issued_t));
    o.set("unsafe_targets_gt", unsafe_gt);
    if (cfg.overlay_every_s > 0) o.raw("overlay_samples", json::array(overlay_samples.begin(), overlay_samples.end())).set("overlay_max_bytes", uint64_t(overlay_max_bytes));
    o.set("instructions_received", me.received).set("instructions_followed", me.followed);
    o.raw("guide", engine.report_json());
    if (gp.understanding != 0) {  // Phase 9: detected doors / windows (absent with the flag off)
      std::vector<std::string> ops;
      for (auto& op : engine.openings()) {
        json::Object d;
        d.set("kind", std::string(op.kind == OpeningKind::Door ? "door" : "window"));
        d.set("x0", r3(op.x0)).set("z0", r3(op.z0)).set("x1", r3(op.x1)).set("z1", r3(op.z1));
        d.set("y0", r3(op.y0)).set("y1", r3(op.y1)).set("conf", r3(op.conf));
        ops.push_back(d.dump());
      }
      o.raw("openings", json::array(ops.begin(), ops.end()));
      o.set("points_stored", uint64_t(engine.points().size()));
    }
    double mins = std::max(dur / 60.0, 1e-9);
    o.set("flap_per_min", r3(engine.planner().switches() / mins));
  }
  if (ueval) {
    json::Object ub, tb, gb;
    int tn = 0, th = 0, gn = 0, gf = 0;
    for (const char* k : {"wall", "corner", "block", "ceiling", "floor"}) {
      const GapEval::T t = geval.truth.count(k) ? geval.truth.at(k) : GapEval::T{};
      tb.raw(k, json::Object().set("n", t.n).set("hit", t.hit).dump());
      tn += t.n, th += t.hit;
    }
    for (const char* k : {"missing_wall", "unseen_corner", "unseen_ceiling", "unseen_floor", "occluded", "weak"}) {
      const GapEval::G g = geval.gaps.count(k) ? geval.gaps.at(k) : GapEval::G{};
      gb.raw(k, json::Object().set("n", g.n).set("false", g.fals).set("far", g.far).dump());
      gn += g.n, gf += g.fals;
    }
    ub.set("checkpoints", geval.checkpoints).raw("truth", tb.dump()).raw("gaps", gb.dump());
    ub.set("truth_n", tn).set("truth_hit", th).set("gaps_n", gn).set("gaps_false", gf);
    static const char* gnames[] = {"missing_wall", "unseen_corner", "unseen_ceiling", "unseen_floor", "occluded", "weak"};
    std::vector<std::string> lg;
    for (const Gap& g : last_gaps) {
      json::Object d;
      d.set("type", gnames[int(g.type)]).set("x0", r3(g.x0)).set("z0", r3(g.z0)).set("x1", r3(g.x1)).set("z1", r3(g.z1));
      d.set("pri", r3(g.pri)).set("why", g.why);
      lg.push_back(d.dump());
    }
    ub.raw("last_gaps", json::array(lg.begin(), lg.end()));
    ub.set("shell_walls", uint64_t(sshell.shell().walls.size())).set("shell_closed", sshell.shell().closed);
    {
      std::vector<std::string> sw;  // the shadow shell as of the last second (debugging aid, also in the report doc)
      for (const ShellWall& sl : sshell.shell().walls) {
        json::Object d;
        d.set("id", sl.id).set("x0", r3(sl.x0)).set("z0", r3(sl.z0)).set("x1", r3(sl.x1)).set("z1", r3(sl.z1)).set("inferred", sl.inferred);
        sw.push_back(d.dump());
      }
      ub.raw("shell", json::array(sw.begin(), sw.end()));
    }
    o.raw("understanding", ub.dump());
  }
  if (oracle) {
    std::vector<std::string> lo, tr;
    for (const Object& ob : lifter.objects()) {
      json::Object d;
      d.set("cls", std::string(object_class_name(ob.cls))).set("x", r3(ob.centre.x())).set("y", r3(ob.centre.y())).set("z", r3(ob.centre.z()));
      d.set("w", r3(ob.size.x())).set("h", r3(ob.size.y())).set("d", r3(ob.size.z())).set("yaw", r3(ob.yaw));
      d.set("conf", r3(ob.conf)).set("n_obs", ob.n_obs);
      lo.push_back(d.dump());
    }
    for (const Box& b : room.boxes) {
      if (!b.cls) continue;
      json::Object d;
      d.set("cls", std::string(object_class_name(ObjectClass(b.cls)))).set("x", r3(0.5 * (b.x0 + b.x1))).set("y", r3(0.5 * b.h));
      d.set("z", r3(0.5 * (b.z0 + b.z1))).set("w", r3(b.x1 - b.x0)).set("h", r3(b.h)).set("d", r3(b.z1 - b.z0));
      tr.push_back(d.dump());
    }
    json::Object od;
    od.raw("objects", json::array(lo.begin(), lo.end())).raw("truth", json::array(tr.begin(), tr.end()));
    od.set("detections", or_dets).set("false_detections", or_false).set("tracks", uint64_t(lifter.tracks()));
    o.raw("oracle", od.dump());
  }
  return o.dump();
}

}  // namespace capturekit::guide::sim
