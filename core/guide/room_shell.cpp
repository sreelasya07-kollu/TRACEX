#include "capturekit/guide/room_shell.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace capturekit::guide {

namespace {

constexpr double kShellPi = 3.14159265358979323846;

struct Part {
  double lo = 0, hi = 0;  // along-line interval of one member plane
  std::string plane;
};

struct Seg {
  int axis = 0;  // 0: the wall is the line x' = off (spans z'), 1: z' = off (spans x')
  int k = 0;     // inward normal: 0 +x', 1 +z', 2 -x', 3 -z' (axis = k % 2)
  double off = 0, lo = 0, hi = 0, wt = 0;
  double omin = 0, omax = 0;  // smallest / largest member offset: a merged line never spans more than merge_off
  int id = 0;            // assigned wall number (assign_ids)
  std::vector<int> nums;  // member plane numbers
  std::vector<Part> parts;
};
using P2 = std::pair<double, double>;

int inward_sign(int k) { return k < 2 ? 1 : -1; }

struct Frame {
  double c, s;  // cos / sin of the Manhattan yaw
  P2 rot(double x, double z) const { return {x * c + z * s, -x * s + z * c}; }
  P2 unrot(double xp, double zp) const { return {xp * c - zp * s, xp * s + zp * c}; }
};

P2 corner_of(const Seg& p, const Seg& q) {  // p and q are on different axes
  double xp = p.axis == 0 ? p.off : q.off, zp = p.axis == 1 ? p.off : q.off;
  return {xp, zp};
}

bool adjacent(const Seg& p, const Seg& q, double tol) {
  if (p.axis == q.axis) return false;
  return q.off >= p.lo - tol && q.off <= p.hi + tol && p.off >= q.lo - tol && p.off <= q.hi + tol;
}

double signed_area(const std::vector<P2>& v) {
  double a = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    auto& p = v[i];
    auto& q = v[(i + 1) % v.size()];
    a += p.first * q.second - q.first * p.second;
  }
  return 0.5 * a;
}

double dist_point_segment(double px, double pz, double x0, double z0, double x1, double z1) {
  double dx = x1 - x0, dz = z1 - z0, l2 = dx * dx + dz * dz;
  double t = l2 > 0 ? std::clamp(((px - x0) * dx + (pz - z0) * dz) / l2, 0.0, 1.0) : 0.0;
  return std::hypot(px - (x0 + t * dx), pz - (z0 + t * dz));
}

void absorb(Seg& a, const Seg& b) {
  double w = a.wt + b.wt;
  a.off = (a.off * a.wt + b.off * b.wt) / w;
  a.lo = std::min(a.lo, b.lo), a.hi = std::max(a.hi, b.hi);
  a.omin = std::min(a.omin, b.omin), a.omax = std::max(a.omax, b.omax);
  a.wt = w;
  a.nums.insert(a.nums.end(), b.nums.begin(), b.nums.end());
  a.parts.insert(a.parts.end(), b.parts.begin(), b.parts.end());
}

int min_num(const Seg& s) { return s.nums.empty() ? 0 : *std::min_element(s.nums.begin(), s.nums.end()); }

/// What a finished wall is made of, for layout_walls() and spans().
struct WallBuild {
  int axis = 0, k = 0;
  double off = 0;
  std::vector<Part> parts;
  bool evidenced = false;
};

}  // namespace

int RoomShell::wall_number(const std::string& plane_id, const WallIds* ids) {
  if (ids) {
    auto it = ids->find(plane_id);
    if (it != ids->end()) return it->second;
  }
  auto [it, fresh] = own_.emplace(plane_id, next_own_);
  if (fresh) ++next_own_;
  return it->second;
}

std::string RoomShell::inferred_id(double angle) {
  // Nearest-match by the side's outward WORLD direction (continuous, so a yaw flip at +-45 degrees, which renumbers the
  // frame's sides, cannot swap ids). A track matches within 45 degrees and only once per update.
  int best = -1;
  double best_d = kShellPi / 4;
  for (size_t i = 0; i < inferred_.size(); ++i) {
    if (inferred_used_[i]) continue;
    double d = std::abs(std::remainder(angle - inferred_[i].first, 2 * kShellPi));
    if (d < best_d) best_d = d, best = int(i);
  }
  if (best < 0) {
    inferred_.push_back({angle, "I" + std::to_string(next_inferred_++)});
    inferred_used_.push_back(1);
    return inferred_.back().second;
  }
  inferred_[size_t(best)].first = angle;
  inferred_used_[size_t(best)] = 1;
  return inferred_[size_t(best)].second;
}

int RoomShell::evidenced_walls() const {
  return int(std::count_if(lwalls_.begin(), lwalls_.end(), [](const LayoutWall& w) { return w.evidenced; }));
}

bool RoomShell::closed_with_evidence() const {
  return shell_.closed && !lwalls_.empty() && evidenced_walls() == int(lwalls_.size());
}

double RoomShell::blocked_m() const {
  double m = 0;
  for (auto& s : spans_)
    if (s.state == 'b') m += s.length();
  return m;
}

void RoomShell::update(const LiveMap& map, const WallIds* ids, const std::vector<Opening>& openings, const LayoutEvidence* ev) {
  shell_ = ShellModel{};
  lwalls_.clear(), spans_.clear(), lines_.clear();
  openings_ = openings;
  for (auto& o : openings_) o.wall.clear();  // early returns must not leak ids the caller passed in
  std::fill(inferred_used_.begin(), inferred_used_.end(), char(0));

  // Ceiling height.
  if (map.has_ceiling()) {
    double h = map.ceiling_y() - map.floor_y();
    if (h >= p_.ceiling_min && h <= p_.ceiling_max) shell_.ceiling_h = h, shell_.ceiling_inferred = false;
  }
  if (shell_.ceiling_inferred) shell_.ceiling_h = p_.default_ceiling;

  // Wall candidates (numbers are assigned to every wall plane, like GuideEngine::assign_wall_ids).
  struct Cand {
    double theta, wt, hw, cx, cz;
    int id;
    std::string pid;
  };
  std::vector<Cand> cands;
  for (const auto& [pid, pl] : map.planes()) {
    if (pl.kind != "wall") continue;
    int num = wall_number(pid, ids);
    Vec3 nh(pl.n.x(), 0, pl.n.z());
    if (nh.norm() < 0.5) continue;
    nh.normalize();
    Vec3 d(-nh.z(), 0, nh.x());
    double hw = 0.5 * (pl.eu * std::abs(pl.u.dot(d)) + pl.ev * std::abs(pl.v.dot(d)));
    if (!std::isfinite(hw) || !pl.c.allFinite() || 2 * hw < p_.min_frag_w) continue;
    cands.push_back({std::atan2(nh.z(), nh.x()), std::max(1e-3, pl.eu * pl.ev), hw, pl.c.x(), pl.c.z(), num, pid});
  }
  if (cands.empty()) {
    yaw_ = 0;
    return;
  }

  // Manhattan yaw (circular mean of 4*theta), then snap.
  double sx = 0, sy = 0;
  for (auto& c : cands) sx += c.wt * std::cos(4 * c.theta), sy += c.wt * std::sin(4 * c.theta);
  yaw_ = (sx * sx + sy * sy > 1e-12) ? std::atan2(sy, sx) / 4.0 : 0.0;
  Frame F{std::cos(yaw_), std::sin(yaw_)};
  const LayoutIndex idx(ev ? *ev : LayoutEvidence{}, yaw_, map.floor_y(), shell_.ceiling_h);

  std::vector<Seg> segs;
  for (auto& c : cands) {
    double rel = c.theta - yaw_, kk = std::round(rel / (kShellPi / 2));
    double dev = std::abs(rel - kk * kShellPi / 2) * 180.0 / kShellPi;
    if (dev > p_.snap_deg) {
      if (2 * c.hw >= p_.big_wall_w) shell_.low_confidence = true;
      continue;
    }
    Seg s;
    s.k = ((int(kk) % 4) + 4) % 4;
    s.axis = s.k % 2;
    auto [cxp, czp] = F.rot(c.cx, c.cz);
    s.off = s.axis == 0 ? cxp : czp;
    s.omin = s.omax = s.off;
    double other = s.axis == 0 ? czp : cxp;
    s.lo = other - c.hw, s.hi = other + c.hw, s.wt = c.wt;
    s.nums = {c.id};
    s.parts = {{s.lo, s.hi, c.pid}};
    segs.push_back(std::move(s));
  }
  if (segs.empty()) {
    shell_.low_confidence = true;
    return;
  }

  // Merge collinear, same-side walls (deterministic order: axis, side, offset, first number). Pass 1: small gaps.
  // Pass 2: bridge longer gaps that the evidence explains (furniture in front, wall points on the line).
  std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) {
    return std::make_tuple(a.axis, a.k, a.off, min_num(a)) < std::make_tuple(b.axis, b.k, b.off, min_num(b));
  });
  auto explained = [&](const Seg& a, const Seg& b, double g0, double g1) {
    const double off = (a.off * a.wt + b.off * b.wt) / (a.wt + b.wt);
    for (auto& p : segs)  // a perpendicular wall reaching into the gap: a T-junction, not one wall
      if (p.axis != a.axis && p.off > g0 + 0.1 && p.off < g1 - 0.1 && off >= p.lo - 0.3 && off <= p.hi + 0.3) return false;
    const int n = std::max(1, int(std::ceil((g1 - g0) / p_.span_step - 1e-9)));
    int ok = 0;
    for (int i = 0; i < n; ++i) {
      double t = g0 + (i + 0.5) * (g1 - g0) / n;
      if (idx.obstacle_at(a.axis, inward_sign(a.k), off, t, p_.strip_d0, p_.strip_d1) ||
          idx.points_on_line(a.axis, off, t - 0.5 * p_.span_step, t + 0.5 * p_.span_step, p_.line_tol) > 0)
        ++ok;
    }
    return ok >= p_.bridge_explained * n;
  };
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1 && !idx.has_points() && !idx.has_nav()) break;
    for (bool changed = true; changed;) {
      changed = false;
      for (size_t i = 0; i < segs.size() && !changed; ++i)
        for (size_t j = i + 1; j < segs.size() && !changed; ++j) {
          Seg &a = segs[i], &b = segs[j];
          if (a.k != b.k || std::abs(a.off - b.off) > p_.merge_off) continue;
          // Member offsets, not the drifting weighted mean: wall 0.00 + fragment 0.12 must not pull in an alcove at 0.24.
          if (std::max(a.omax, b.omax) - std::min(a.omin, b.omin) > p_.merge_off) continue;
          const double g0 = std::min(a.hi, b.hi), g1 = std::max(a.lo, b.lo);
          if (g1 - g0 > (pass == 0 ? p_.merge_gap : p_.bridge_max)) continue;
          if (pass == 1 && g1 - g0 > p_.merge_gap && !explained(a, b, g0, g1)) continue;
          absorb(a, b);
          segs.erase(segs.begin() + long(j));
          changed = true;
        }
    }
  }
  segs.erase(std::remove_if(segs.begin(), segs.end(), [&](const Seg& s) { return s.hi - s.lo < p_.min_wall_w; }), segs.end());
  if (segs.empty()) {
    shell_.low_confidence = true;
    return;
  }

  // Stable line ids: smallest of the member plane numbers and the numbers of previous lines this one continues. A track
  // continues only its NEAREST-offset matching line, so a parallel alcove line cannot take a re-id'd wall's id.
  auto world_angle = [&](int k) { return std::remainder(yaw_ + k * kShellPi / 2, 2 * kShellPi); };
  {
    std::vector<std::vector<int>> cand(segs.size());
    for (size_t i = 0; i < segs.size(); ++i) cand[i] = segs[i].nums;
    for (auto& t : tracks_) {
      int best = -1;
      double best_d = 0;
      for (size_t i = 0; i < segs.size(); ++i) {
        const Seg& s = segs[i];
        if (std::abs(std::remainder(world_angle(s.k) - t.ang, 2 * kShellPi)) > p_.track_deg * kShellPi / 180.0) continue;
        auto [ax, az] = F.rot(t.x0, t.z0);
        auto [bx, bz] = F.rot(t.x1, t.z1);
        double toff = s.axis == 0 ? 0.5 * (ax + bx) : 0.5 * (az + bz);
        double tlo = s.axis == 0 ? std::min(az, bz) : std::min(ax, bx), thi = s.axis == 0 ? std::max(az, bz) : std::max(ax, bx);
        const double d = std::abs(toff - s.off);
        if (d > p_.track_off || std::min(thi, s.hi) - std::max(tlo, s.lo) <= 0) continue;
        if (best < 0 || d < best_d) best = int(i), best_d = d;
      }
      if (best >= 0) cand[size_t(best)].push_back(t.num);
    }
    for (auto& c : cand) {
      std::sort(c.begin(), c.end());
      c.erase(std::unique(c.begin(), c.end()), c.end());
    }
    std::vector<size_t> order(segs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto own_first = [&](size_t i) {  // 0 when the smallest candidate is one of the line's own planes
      return std::find(segs[i].nums.begin(), segs[i].nums.end(), cand[i].front()) != segs[i].nums.end() ? 0 : 1;
    };
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return std::make_tuple(cand[a].front(), own_first(a), segs[a].axis, segs[a].k, segs[a].off) <
             std::make_tuple(cand[b].front(), own_first(b), segs[b].axis, segs[b].k, segs[b].off);
    });
    std::vector<int> taken;
    for (size_t i : order) {
      int pick = 0;
      for (int c : cand[i])
        if (std::find(taken.begin(), taken.end(), c) == taken.end()) {
          pick = c;
          break;
        }
      if (pick == 0) pick = kFreshBase + next_fresh_++;  // degenerate: every candidate claimed elsewhere -> fresh number
      segs[i].id = pick;
      taken.push_back(pick);
    }
  }
  for (auto& s : segs) lines_.push_back({s.axis, s.k, s.off, s.lo, s.hi, "W" + std::to_string(s.id), {}, false});
  for (size_t i = 0; i < segs.size(); ++i)
    for (auto& p : segs[i].parts) lines_[i].planes.push_back(p.plane);

  auto wid = [](int n) { return "W" + std::to_string(n); };
  auto world = [&](double xp, double zp) { return F.unrot(xp, zp); };
  std::vector<WallBuild> builds;  // parallel to shell_.walls

  // Closed loop of 4 (rectangle) or 6 (L) wall lines?
  const size_t n = segs.size();
  bool chained = false;
  if (n == 4 || n == 6) {
    std::vector<std::vector<int>> nb(n);
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < n; ++j)
        if (i != j && adjacent(segs[i], segs[j], p_.corner_tol)) nb[i].push_back(int(j));
    bool deg2 = true;
    for (auto& v : nb) deg2 = deg2 && v.size() == 2;
    std::vector<int> order;
    if (deg2) {
      int prev = -1, cur = 0;
      do {
        order.push_back(cur);
        int nx = nb[size_t(cur)][0] != prev ? nb[size_t(cur)][0] : nb[size_t(cur)][1];
        prev = cur, cur = nx;
      } while (cur != 0 && order.size() <= n);
      deg2 = order.size() == n;
    }
    if (deg2) {
      auto corners = [&](const std::vector<int>& o) {
        std::vector<P2> pts;
        for (size_t k = 0; k < n; ++k)
          pts.push_back(corner_of(segs[size_t(o[(k + n - 1) % n])], segs[size_t(o[k])]));
        return pts;
      };
      if (signed_area(corners(order)) < 0) std::reverse(order.begin(), order.end());
      size_t first = 0;
      for (size_t k = 1; k < n; ++k)
        if (segs[size_t(order[k])].id < segs[size_t(order[first])].id) first = k;
      std::rotate(order.begin(), order.begin() + long(first), order.end());
      auto pts = corners(order);
      if (std::abs(signed_area(pts)) >= 1.0) {
        chained = true;
        for (auto& [xp, zp] : pts) shell_.corners.push_back(world(xp, zp));
        for (size_t k = 0; k < n; ++k) {
          auto [x0, z0] = shell_.corners[k];
          auto [x1, z1] = shell_.corners[(k + 1) % n];
          const Seg& s = segs[size_t(order[k])];
          shell_.walls.push_back({wid(s.id), x0, z0, x1, z1, false});
          builds.push_back({s.axis, s.k, s.off, s.parts, true});
        }
        shell_.closed = true;
      }
    }
  }

  if (!chained) {
    // Manhattan bounding rectangle, flagged low-confidence.
    shell_.low_confidence = true;
    double xlo = 1e18, xhi = -1e18, zlo = 1e18, zhi = -1e18;
    auto addx = [&](double v) { xlo = std::min(xlo, v), xhi = std::max(xhi, v); };
    auto addz = [&](double v) { zlo = std::min(zlo, v), zhi = std::max(zhi, v); };
    for (auto& s : segs) {
      if (s.axis == 0) addx(s.off), addz(s.lo), addz(s.hi);
      else addz(s.off), addx(s.lo), addx(s.hi);
    }
    if (map.camera_bounds().valid() && (xhi - xlo < p_.min_dim || zhi - zlo < p_.min_dim)) {
      const Aabb& b = map.camera_bounds();
      for (double x : {b.lo.x(), b.hi.x()})
        for (double z : {b.lo.z(), b.hi.z()}) {
          auto [xp, zp] = F.rot(x, z);
          if (xhi - xlo < p_.min_dim) addx(xp);
          if (zhi - zlo < p_.min_dim) addz(zp);
        }
    }
    if (xhi - xlo < p_.min_dim || zhi - zlo < p_.min_dim) {  // too little to outline: observed walls only
      for (auto& s : segs) {
        auto a = s.axis == 0 ? world(s.off, s.lo) : world(s.lo, s.off);
        auto b = s.axis == 0 ? world(s.off, s.hi) : world(s.hi, s.off);
        shell_.walls.push_back({wid(s.id), a.first, a.second, b.first, b.second, false});
        builds.push_back({s.axis, s.k, s.off, s.parts, true});
      }
    } else {
      const int side_axis[4] = {1, 0, 1, 0};
      const int side_k[4] = {1, 2, 3, 0};      // inward normal of a wall on each side
      const int side_out[4] = {-1, 1, 1, -1};  // outward direction along the side's axis
      double side_off[4] = {zlo, xhi, zhi, xlo};
      auto hits = [&](int k, double off) {
        std::vector<const Seg*> v;
        for (auto& s : segs)
          if (s.axis == side_axis[k] && s.k == side_k[k] && std::abs(s.off - off) <= p_.side_match) v.push_back(&s);
        return v;
      };
      // Plane-less sides: a wall line from sparse points beyond the outline (outside the camera path), refined by the
      // floor plane edge. Searched on the rectangle as observed, applied together.
      bool from_pts[4] = {false, false, false, false};
      if (idx.has_points()) {
        double cam_lo[2] = {1e18, 1e18}, cam_hi[2] = {-1e18, -1e18};
        if (map.camera_bounds().valid()) {
          const Aabb& b = map.camera_bounds();
          for (double x : {b.lo.x(), b.hi.x()})
            for (double z : {b.lo.z(), b.hi.z()}) {
              auto [xp, zp] = F.rot(x, z);
              cam_lo[0] = std::min(cam_lo[0], xp), cam_hi[0] = std::max(cam_hi[0], xp);
              cam_lo[1] = std::min(cam_lo[1], zp), cam_hi[1] = std::max(cam_hi[1], zp);
            }
        }
        double found[4];
        for (int k = 0; k < 4; ++k) {
          if (!hits(k, side_off[k]).empty()) continue;
          const int ax = side_axis[k], o = side_out[k];
          const double far = side_off[k] + o * p_.infer_out;
          double near = side_off[k] - o * p_.infer_in;
          if (cam_lo[ax] <= cam_hi[ax])
            near = o < 0 ? std::min(near, cam_lo[ax] - p_.cam_clear) : std::max(near, cam_hi[ax] + p_.cam_clear);
          if ((far - near) * o <= 0) continue;
          const double alo = (ax == 0 ? zlo : xlo) + 0.2, ahi = (ax == 0 ? zhi : xhi) - 0.2;
          double off = 0;
          if (!idx.infer_line(ax, near, far, alo, ahi, p_.infer_min_pts, p_.infer_min_spread, off)) continue;
          bool have_edge = false;
          double edge = 0;
          for (auto& [_, pl] : map.planes()) {  // the floor edge on this side (big floor planes near the floor level)
            if (pl.kind != "floor" || std::fabs(pl.n.y()) < 0.9 || pl.eu * pl.ev < 1.0) continue;
            if (std::fabs(pl.c.y() - map.floor_y()) > 0.15) continue;
            for (int su : {-1, 1})
              for (int sv : {-1, 1}) {
                Vec3 q = pl.c + pl.u * (0.5 * su * pl.eu) + pl.v * (0.5 * sv * pl.ev);
                auto [qx, qz] = F.rot(q.x(), q.z());
                double v = ax == 0 ? qx : qz;
                if (!have_edge || v * o > edge * o) edge = v, have_edge = true;
              }
          }
          if (have_edge && std::abs(edge - off) <= p_.floor_edge_tol) off = 0.5 * (off + edge);
          found[k] = off, from_pts[k] = true;
        }
        for (int k = 0; k < 4; ++k)
          if (from_pts[k]) side_off[k] = found[k];
        zlo = side_off[0], xhi = side_off[1], zhi = side_off[2], xlo = side_off[3];
      }
      const P2 q[4] = {{xlo, zlo}, {xhi, zlo}, {xhi, zhi}, {xlo, zhi}};
      for (auto& [xp, zp] : q) shell_.corners.push_back(world(xp, zp));
      const P2 out_n[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};  // outward normals in the Manhattan frame
      for (int k = 0; k < 4; ++k) {
        auto [wx, wz] = F.unrot(out_n[k].first, out_n[k].second);
        const double angle = std::atan2(wz, wx);
        auto hv = hits(k, side_off[k]);
        const Seg* hit = nullptr;
        WallBuild wb{side_axis[k], side_k[k], side_off[k], {}, false};
        for (const Seg* s : hv) {
          if (!hit || s->id < hit->id) hit = s;
          wb.parts.insert(wb.parts.end(), s->parts.begin(), s->parts.end());
        }
        wb.evidenced = hit != nullptr || from_pts[k];
        auto [x0, z0] = shell_.corners[size_t(k)];
        auto [x1, z1] = shell_.corners[size_t((k + 1) % 4)];
        shell_.walls.push_back({hit ? wid(hit->id) : inferred_id(angle), x0, z0, x1, z1, hit == nullptr});
        builds.push_back(std::move(wb));
        if (from_pts[k] && !hit) {
          double lo = side_axis[k] == 0 ? zlo : xlo, hi = side_axis[k] == 0 ? zhi : xhi;
          lines_.push_back({side_axis[k], side_k[k], side_off[k], lo, hi, shell_.walls.back().id, {}, true});
        }
      }
      shell_.closed = true;
      // Observed lines that are no side of the rectangle (an inner wall, a jog) stay listed as walls, off the outline.
      for (auto& s : segs) {
        bool on_side = false;
        for (int k = 0; k < 4 && !on_side; ++k)
          for (const Seg* h : hits(k, side_off[k])) on_side = on_side || h == &s;
        if (on_side) continue;
        auto a = s.axis == 0 ? world(s.off, s.lo) : world(s.lo, s.off);
        auto b = s.axis == 0 ? world(s.off, s.hi) : world(s.hi, s.off);
        shell_.walls.push_back({wid(s.id), a.first, a.second, b.first, b.second, false});
        builds.push_back({s.axis, s.k, s.off, s.parts, true});
      }
    }
  }

  // Remember every W line (ids follow lines across plane re-ids); bounded.
  for (auto& s : segs) {
    auto a = s.axis == 0 ? world(s.off, s.lo) : world(s.lo, s.off);
    auto b = s.axis == 0 ? world(s.off, s.hi) : world(s.hi, s.off);
    Track t{world_angle(s.k), a.first, a.second, b.first, b.second, s.id};
    auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& x) { return x.num == s.id; });
    if (it != tracks_.end()) *it = t;
    else tracks_.push_back(t);
  }
  if (tracks_.size() > 256) tracks_.erase(tracks_.begin(), tracks_.begin() + long(tracks_.size() - 256));

  // Per-wall layout info and spans.
  for (size_t i = 0; i < shell_.walls.size(); ++i) {
    const ShellWall& w = shell_.walls[i];
    const WallBuild& b = builds[i];
    LayoutWall lw;
    lw.id = w.id, lw.num = layout_wall_num(w.id), lw.evidenced = b.evidenced;
    for (auto& p : b.parts) lw.planes.push_back(p.plane);
    std::sort(lw.planes.begin(), lw.planes.end());
    lw.planes.erase(std::unique(lw.planes.begin(), lw.planes.end()), lw.planes.end());
    lwalls_.push_back(std::move(lw));

    auto [ax, az] = F.rot(w.x0, w.z0);
    auto [bx, bz] = F.rot(w.x1, w.z1);
    const double t0 = b.axis == 0 ? az : ax, t1 = b.axis == 0 ? bz : bx;
    const double L = std::abs(t1 - t0);
    if (L < 1e-6) continue;
    const int ns = std::max(1, int(std::lround(L / p_.span_step)));
    std::vector<char> st(size_t(ns), 'm');
    for (int j = 0; j < ns; ++j) {
      const double t = t0 + (t1 - t0) * (j + 0.5) / ns, h = 0.5 * L / ns;
      bool on_plane = false;
      for (auto& p : b.parts) on_plane = on_plane || (t >= p.lo && t <= p.hi);
      if (on_plane) st[size_t(j)] = 'o';
      else if (idx.obstacle_at(b.axis, inward_sign(b.k), b.off, t, p_.strip_d0, p_.strip_d1)) st[size_t(j)] = 'b';
      else if (idx.points_on_line(b.axis, b.off, t - h, t + h, p_.line_tol) > 0) st[size_t(j)] = 'o';
    }
    // Run-length, then fold runs shorter than span_min into a neighbour (left first).
    struct Run {
      int a, b;
      char s;
    };
    std::vector<Run> runs;
    for (int j = 0; j < ns; ++j)
      if (runs.empty() || runs.back().s != st[size_t(j)]) runs.push_back({j, j + 1, st[size_t(j)]});
      else runs.back().b = j + 1;
    const int min_samples = std::max(1, int(std::ceil(p_.span_min / (L / ns) - 1e-9)));
    for (size_t r = 0; r < runs.size() && runs.size() > 1;) {
      if (runs[r].b - runs[r].a >= min_samples) {
        ++r;
        continue;
      }
      if (r > 0) runs[r - 1].b = runs[r].b;
      else runs[r + 1].a = runs[r].a;
      runs.erase(runs.begin() + long(r));
      for (size_t m = 1; m < runs.size();)  // re-join equal neighbours
        if (runs[m].s == runs[m - 1].s) runs[m - 1].b = runs[m].b, runs.erase(runs.begin() + long(m));
        else ++m;
      r = 0;
    }
    for (auto& r : runs) {
      const double fa = double(r.a) / ns, fb = double(r.b) / ns;
      spans_.push_back({w.id, w.x0 + fa * (w.x1 - w.x0), w.z0 + fa * (w.z1 - w.z0), w.x0 + fb * (w.x1 - w.x0),
                        w.z0 + fb * (w.z1 - w.z0), r.s});
    }
  }

  // Attach openings to the nearest observed wall.
  for (auto& o : openings_) {
    double mx = 0.5 * (o.x0 + o.x1), mz = 0.5 * (o.z0 + o.z1), best = p_.attach_dist;
    o.wall.clear();
    for (auto& w : shell_.walls) {
      if (w.inferred) continue;
      double d = dist_point_segment(mx, mz, w.x0, w.z0, w.x1, w.z1);
      if (d <= best) best = d, o.wall = w.id;
    }
  }
}

}  // namespace capturekit::guide
