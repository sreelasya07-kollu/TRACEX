#include "capturekit/guide/gap_finder.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace capturekit::guide {

namespace {

using P2 = std::pair<double, double>;

bool in_polygon(const std::vector<P2>& poly, double x, double z) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const auto [xi, zi] = poly[i];
    const auto [xj, zj] = poly[j];
    if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
  }
  return in;
}

double dist_seg(double px, double pz, double ax, double az, double bx, double bz) {
  const double dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
  const double t = l2 > 0 ? std::clamp(((px - ax) * dx + (pz - az) * dz) / l2, 0.0, 1.0) : 0.0;
  return std::hypot(px - (ax + t * dx), pz - (az + t * dz));
}

struct Seg2 {
  double x0, z0, x1, z1;
};

/// Horizontal footprint of a wall plane (the segment it spans at its centre), the same width rule as RoomShell.
bool wall_footprint(const Plane& pl, Seg2* out) {
  Vec3 nh(pl.n.x(), 0, pl.n.z());
  if (nh.norm() < 0.5) return false;
  nh.normalize();
  const Vec3 d(-nh.z(), 0, nh.x());
  const double hw = 0.5 * (pl.eu * std::abs(pl.u.dot(d)) + pl.ev * std::abs(pl.v.dot(d)));
  *out = {pl.c.x() - d.x() * hw, pl.c.z() - d.z() * hw, pl.c.x() + d.x() * hw, pl.c.z() + d.z() * hw};
  return true;
}

double seen_of(const Coverage* c, const std::string& id) {
  if (!c) return 0.0;
  const CoverageStats s = c->stats(id);
  return s.valid ? s.seen_fraction() : 0.0;
}

}  // namespace

std::vector<VirtualSurface> GapFinder::virtual_surfaces(const ShellModel& shell, const LiveMap& map) {
  std::vector<VirtualSurface> out;
  if (shell.corners.size() < 3) return out;
  const double fy = map.has_floor() ? map.floor_y() : 0.0, h = shell.ceiling_h;
  double mx = 0, mz = 0, xlo = 1e18, xhi = -1e18, zlo = 1e18, zhi = -1e18;
  for (auto& [x, z] : shell.corners) {
    mx += x, mz += z;
    xlo = std::min(xlo, x), xhi = std::max(xhi, x), zlo = std::min(zlo, z), zhi = std::max(zhi, z);
  }
  mx /= double(shell.corners.size()), mz /= double(shell.corners.size());
  for (const ShellWall& w : shell.walls) {
    if (!w.inferred) continue;
    const double L = std::hypot(w.x1 - w.x0, w.z1 - w.z0);
    if (L < 0.3) continue;
    VirtualSurface s;
    s.id = "v:" + w.id;
    const double cx = 0.5 * (w.x0 + w.x1), cz = 0.5 * (w.z0 + w.z1);
    s.c = Vec3(cx, fy + 0.5 * h, cz);
    s.u = Vec3((w.x1 - w.x0) / L, 0, (w.z1 - w.z0) / L), s.v = Vec3::UnitY();
    s.n = Vec3(-s.u.z(), 0, s.u.x());
    if (s.n.dot(Vec3(mx - cx, 0, mz - cz)) < 0) s.n = -s.n;  // faces the room
    s.eu = L, s.ev = h;
    out.push_back(s);
  }
  auto flat = [&](const char* id, double y, double ny) {
    VirtualSurface s;
    s.id = id;
    s.c = Vec3(0.5 * (xlo + xhi), y, 0.5 * (zlo + zhi));
    s.n = Vec3(0, ny, 0), s.u = Vec3::UnitX(), s.v = Vec3::UnitZ();
    s.eu = xhi - xlo, s.ev = zhi - zlo;
    out.push_back(s);
  };
  if (shell.ceiling_inferred) flat("v:ceiling", fy + h, -1.0);
  if (!map.has_floor()) flat("v:floor", fy, 1.0);
  return out;
}

std::vector<Gap> GapFinder::find(const GapInputs& in) const {
  std::vector<Gap> out;
  if (!in.shell || !in.map || in.shell->walls.empty()) return out;
  const ShellModel& sh = *in.shell;
  const LiveMap& map = *in.map;
  static const std::vector<Object> kNoObjects;
  static const std::vector<Opening> kNoOpenings;
  const std::vector<Object>& objects = in.objects ? *in.objects : kNoObjects;
  const std::vector<Opening>& openings = in.openings ? *in.openings : kNoOpenings;

  // Footprints of the observed wall planes.
  std::vector<Seg2> feet;
  for (const auto& [id, pl] : map.planes()) {
    Seg2 s;
    if (pl.kind == "wall" && wall_footprint(pl, &s)) feet.push_back(s);
  }
  auto add = [&](GapType t, double x0, double z0, double x1, double z1, double pri) {
    Gap g;
    g.type = t, g.x0 = x0, g.z0 = z0, g.x1 = x1, g.z1 = z1, g.pri = std::clamp(pri, 0.0, 1.0);
    out.push_back(g);
  };

  // MissingWall: an inferred wall nobody has looked at (only once the room is outlined by enough observed walls).
  const int observed_walls = int(std::count_if(sh.walls.begin(), sh.walls.end(), [](const ShellWall& w) { return !w.inferred; }));
  for (const ShellWall& w : sh.walls) {
    if (!w.inferred) continue;
    if (observed_walls < p_.min_observed_walls) break;
    const double seen = seen_of(in.vcov, "v:" + w.id);
    if (seen >= p_.looked_frac) continue;
    add(GapType::MissingWall, w.x0, w.z0, w.x1, w.z1, p_.pri_wall * (1.0 - 0.5 * seen / std::max(p_.looked_frac, 1e-9)));
  }

  // UnseenCorner: a corner between two observed walls that no wall plane reaches.
  const size_t n = sh.corners.size();
  if (n >= 3 && sh.walls.size() == n) {
    for (size_t k = 0; k < n; ++k) {
      if (sh.walls[(k + n - 1) % n].inferred || sh.walls[k].inferred) continue;
      const auto [cx, cz] = sh.corners[k];
      double d = 1e18;
      for (const Seg2& s : feet) d = std::min(d, dist_seg(cx, cz, s.x0, s.z0, s.x1, s.z1));
      if (d <= p_.corner_unseen || d > p_.corner_max) continue;
      add(GapType::UnseenCorner, cx - 0.3, cz - 0.3, cx + 0.3, cz + 0.3, p_.pri_corner);
    }
  }

  // UnseenCeiling / UnseenFloor: no plane, and the virtual grid was not looked at.
  if (n >= 3) {
    double xlo = 1e18, xhi = -1e18, zlo = 1e18, zhi = -1e18;
    for (auto& [x, z] : sh.corners) xlo = std::min(xlo, x), xhi = std::max(xhi, x), zlo = std::min(zlo, z), zhi = std::max(zhi, z);
    if (sh.ceiling_inferred && seen_of(in.vcov, "v:ceiling") < p_.looked_frac) add(GapType::UnseenCeiling, xlo, zlo, xhi, zhi, p_.pri_ceiling);
    if (!map.has_floor() && seen_of(in.vcov, "v:floor") < p_.looked_frac) add(GapType::UnseenFloor, xlo, zlo, xhi, zhi, p_.pri_floor);
  }

  // Occluded: floor ring around a lifted object with no keyframe view (needs the real coverage grids).
  if (in.cov && !objects.empty()) {
    std::vector<std::string> floors;
    for (const auto& [id, pl] : map.planes())
      if (pl.kind == "floor") floors.push_back(id);
    const double fy = map.has_floor() ? map.floor_y() : 0.0;
    for (const Object& ob : objects) {
      if (object_class_wall_mounted(ob.cls)) continue;
      const double r = 0.5 * std::hypot(ob.size.x(), ob.size.z()) + p_.ring_margin;
      int valid = 0, unseen = 0;
      double xlo = 1e18, xhi = -1e18, zlo = 1e18, zhi = -1e18;
      for (int i = 0; i < p_.ring_samples; ++i) {
        const double a = 2 * kPi * double(i) / double(p_.ring_samples);
        const double x = ob.centre.x() + r * std::cos(a), z = ob.centre.z() + r * std::sin(a);
        if (n >= 3 && !in_polygon(sh.corners, x, z)) continue;  // outside the room (behind a wall)
        bool inside_other = false;
        for (const Object& o2 : objects)
          if (&o2 != &ob && std::hypot(o2.centre.x() - x, o2.centre.z() - z) < 0.5 * std::hypot(o2.size.x(), o2.size.z())) inside_other = true;
        if (inside_other) continue;
        ++valid;
        int views = 0;
        for (const auto& id : floors) views = std::max(views, in.cov->views_at(id, Vec3(x, fy, z)));
        if (views == 0) {
          ++unseen;
          xlo = std::min(xlo, x), xhi = std::max(xhi, x), zlo = std::min(zlo, z), zhi = std::max(zhi, z);
        }
      }
      if (valid < p_.ring_min_samples || double(unseen) < p_.ring_unseen_frac * double(valid)) continue;
      add(GapType::Occluded, xlo, zlo, xhi, zhi, p_.pri_occluded + 0.2 * double(unseen) / double(valid));
    }
  }

  // Weak: observed surfaces seen but too thinly.
  if (in.cov) {
    for (const std::string& id : in.cov->plane_ids()) {
      auto it = map.planes().find(id);
      if (it == map.planes().end() || !it->second.surface()) continue;
      const CoverageStats st = in.cov->stats(id);
      if (st.valid < p_.weak_min_cells || st.seen_fraction() < p_.weak_seen || st.completeness() >= p_.weak_suff) continue;
      const Plane& pl = it->second;
      Seg2 s{};
      if (pl.kind == "wall") {
        if (!wall_footprint(pl, &s)) continue;
      } else {
        const double hx = 0.5 * (pl.eu * std::abs(pl.u.x()) + pl.ev * std::abs(pl.v.x()));
        const double hz = 0.5 * (pl.eu * std::abs(pl.u.z()) + pl.ev * std::abs(pl.v.z()));
        s = {pl.c.x() - hx, pl.c.z() - hz, pl.c.x() + hx, pl.c.z() + hz};
      }
      add(GapType::Weak, s.x0, s.z0, s.x1, s.z1, p_.pri_weak * (1.0 - st.completeness()));
    }
  }

  std::stable_sort(out.begin(), out.end(), [](const Gap& a, const Gap& b) {
    return std::make_tuple(-a.pri, int(a.type), a.x0, a.z0) < std::make_tuple(-b.pri, int(b.type), b.x0, b.z0);
  });
  if (out.size() > p_.max_gaps) out.resize(p_.max_gaps);
  for (Gap& g : out) g.why = labeler_.label(g, openings, objects, in.user);
  return out;
}

}  // namespace capturekit::guide
