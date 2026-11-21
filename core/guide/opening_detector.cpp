#include "capturekit/guide/opening_detector.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace capturekit::guide {

namespace {
double clamp01(double x) { return std::max(0.0, std::min(1.0, x)); }

struct Cand {
  int i0, i1, j0, j1;
  OpeningKind kind;
  double conf;
};
}  // namespace

std::vector<Opening> OpeningDetector::detect_wall(const Plane& wall, bool has_floor, double floor_y, const Vec3& room_side,
                                                  const SparsePointStore& pts, const ViewsFn& views) const {
  std::vector<Opening> out;
  const OpeningParams& P = p_;
  // Horizontal wall normal facing the room and the along-wall axis.
  Vec3 n(wall.n.x(), 0, wall.n.z());
  if (!(n.norm() > 0.5) || !wall.c.allFinite()) return out;
  n.normalize();
  if ((room_side - wall.c).dot(n) < -0.5) n = -n;  // trust the platform normal unless the cameras are clearly behind it
  const Vec3 hz = n.cross(Vec3::UnitY()).normalized();
  const double half_w = 0.5 * (wall.eu * std::fabs(wall.u.dot(hz)) + wall.ev * std::fabs(wall.v.dot(hz)));
  const double half_h = 0.5 * (wall.eu * std::fabs(wall.u.y()) + wall.ev * std::fabs(wall.v.y()));
  if (2 * half_w < P.min_wall_w || 2 * half_h < P.min_wall_h) return out;
  const double y_top = wall.c.y() + half_h;
  double y_lo = wall.c.y() - half_h;
  const bool at_floor = has_floor && y_lo < floor_y + P.floor_snap;
  if (at_floor) y_lo = floor_y;
  const double ref_floor = has_floor ? floor_y : y_lo;
  const int nu = int(std::lround(2 * half_w / P.cell)), nv = int(std::lround((y_top - y_lo) / P.cell));
  if (nu < 5 || nv < 5) return out;
  const auto idx = [nu](int i, int j) { return size_t(j) * size_t(nu) + size_t(i); };

  std::vector<uint32_t> solid(size_t(nu) * size_t(nv), 0), through(solid.size(), 0);
  pts.for_each([&](const SparsePointStore::Voxel& vx) {
    const Vec3 p = vx.mean();
    const Vec3 d = p - wall.c;
    const double dn = d.dot(n);
    const int i = int(std::floor((d.dot(hz) + half_w) / P.cell)), j = int(std::floor((p.y() - y_lo) / P.cell));
    if (i < 0 || i >= nu || j < 0 || j >= nv) return;
    if (has_floor && p.y() < floor_y + P.floor_band) return;
    if (std::fabs(dn) <= P.on_plane_tol) ++solid[idx(i, j)];
    else if (dn < -P.through_min && dn > -P.through_max) ++through[idx(i, j)];
  });

  // Despeckle: a weak solid cell (<= 2 points, >= 3 empty 4-neighbours; 1 point, >= 2) is noise (a stray point across
  // an opening edge); judged on the raw grid, one pass.
  {
    std::vector<uint32_t> s2 = solid;
    for (int j = 0; j < nv; ++j)
      for (int i = 0; i < nu; ++i) {
        if (!solid[idx(i, j)] || solid[idx(i, j)] > 2) continue;
        int e = 0;
        const int nb[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
        for (auto& q : nb) e += q[0] >= 0 && q[0] < nu && q[1] >= 0 && q[1] < nv && !solid[idx(q[0], q[1])];
        if (e >= 3 || (e >= 2 && solid[idx(i, j)] == 1)) s2[idx(i, j)] = 0;
      }
    solid.swap(s2);
  }
  // Close isolated holes left by sparse sampling (computed on the despeckled grid, one pass).
  std::vector<uint8_t> full(solid.size(), 0), vis(solid.size(), 1);
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      if (solid[idx(i, j)]) { full[idx(i, j)] = 1; continue; }
      int k = 0;
      for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
          int a = i + di, b = j + dj;
          if ((di || dj) && a >= 0 && a < nu && b >= 0 && b < nv && solid[idx(a, b)]) ++k;
        }
      full[idx(i, j)] = k >= P.close_min_neighbours;
    }
  // Free-space evidence: an empty cell is free only with >= min_cell_views views, else unknown (solid cells are
  // observed by definition).
  if (views)
    for (int j = 0; j < nv; ++j)
      for (int i = 0; i < nu; ++i)
        if (!solid[idx(i, j)])
          vis[idx(i, j)] =
              views(wall.c + hz * (-half_w + (i + 0.5) * P.cell) + Vec3(0, y_lo + (j + 0.5) * P.cell - wall.c.y(), 0)) >=
              P.min_cell_views;

  // Per-row prefix counts of non-empty cells: a rectangle is empty when every row has zero in its column range.
  std::vector<int> pre(size_t(nv) * size_t(nu + 1), 0);
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) pre[size_t(j) * size_t(nu + 1) + size_t(i + 1)] = pre[size_t(j) * size_t(nu + 1) + size_t(i)] + full[idx(i, j)];
  const auto row_empty = [&](int j, int i0, int i1) {
    return pre[size_t(j) * size_t(nu + 1) + size_t(i1 + 1)] - pre[size_t(j) * size_t(nu + 1) + size_t(i0)] == 0;
  };
  const auto col_empty = [&](int i, int j0, int j1) {
    for (int j = j0; j <= j1; ++j)
      if (full[idx(i, j)]) return false;
    return true;
  };

  // Maximal empty rectangles (cannot grow in any direction), filtered by the door / window rules.
  const int max_cols = int(std::ceil(std::max(P.door_max_w, P.win_max_w) / P.cell)) + 1;
  std::vector<Cand> cands;
  for (int i0 = 1; i0 < nu - 1; ++i0)
    for (int i1 = i0; i1 < nu - 1 && i1 - i0 < max_cols; ++i1)
      for (int j = 0; j < nv;) {
        if (!row_empty(j, i0, i1)) { ++j; continue; }
        int j0 = j;
        while (j < nv && row_empty(j, i0, i1)) ++j;
        const int j1 = j - 1;
        // Unbounded: touches the top of the observed wall, or can grow sideways (then the wider one is the candidate).
        if (j1 == nv - 1 || col_empty(i0 - 1, j0, j1) || col_empty(i1 + 1, j0, j1)) continue;
        const bool floor_touch = j0 == 0;
        if (floor_touch && !at_floor) continue;
        const double w = (i1 - i0 + 1) * P.cell, h = (j1 - j0 + 1) * P.cell;
        const double bottom = y_lo + j0 * P.cell - ref_floor;
        OpeningKind kind;
        if (floor_touch) {
          if (w < P.door_min_w || w > P.door_max_w || h < P.door_min_h) continue;
          kind = OpeningKind::Door;
        } else {
          if (bottom < P.win_sill_min || bottom > P.win_sill_max || w < P.win_min_w || w > P.win_max_w || h < P.win_min_h)
            continue;
          kind = OpeningKind::Window;
        }
        // Evidence: the inside was in view, the one-cell ring around it is wall, things show through it.
        int n_in = 0, n_vis = 0;
        uint32_t thr = 0;
        for (int b = j0; b <= j1; ++b)
          for (int a = i0; a <= i1; ++a) ++n_in, n_vis += vis[idx(a, b)], thr += through[idx(a, b)];
        const double seen_frac = double(n_vis) / n_in;
        if (seen_frac < P.min_seen) continue;
        int ring = 0, known = 0, wallc = 0;
        // Probe two cells outward (sparse points): wall if either is solid or closed, known if wall or in view.
        auto probe = [&](int a, int b, int da, int db) {
          if (a < 0 || a >= nu || b < 0 || b >= nv) return;
          ++ring;
          const int a2 = a + da, b2 = b + db;
          const bool in2 = a2 >= 0 && a2 < nu && b2 >= 0 && b2 < nv;
          if (full[idx(a, b)] || (in2 && full[idx(a2, b2)])) ++known, ++wallc;
          else if (vis[idx(a, b)]) ++known;
        };
        for (int a = i0; a <= i1; ++a) {
          probe(a, j1 + 1, 0, 1);
          if (!floor_touch) probe(a, j0 - 1, 0, -1);
        }
        for (int b = j0; b <= j1; ++b) probe(i0 - 1, b, -1, 0), probe(i1 + 1, b, 1, 0);
        if (!ring || double(known) / ring < P.min_ring_known) continue;
        const double ring_fill = double(wallc) / known;
        if (ring_fill < P.min_ring_fill) continue;
        const double conf = clamp01(0.5 * ring_fill + 0.3 * seen_frac + 0.2 * clamp01(double(thr) / 3.0));
        if (conf < P.min_conf) continue;
        cands.push_back({i0, i1, j0, j1, kind, conf});
      }

  // Overlapping candidates: keep the most confident, then the largest (deterministic tie-break by position).
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    const int aa = (a.i1 - a.i0 + 1) * (a.j1 - a.j0 + 1), ab = (b.i1 - b.i0 + 1) * (b.j1 - b.j0 + 1);
    return std::tie(b.conf, ab, a.i0, a.j0, a.i1, a.j1) < std::tie(a.conf, aa, b.i0, b.j0, b.i1, b.j1);
  });
  std::vector<Cand> keep;
  for (auto& c : cands) {
    bool overlap = false;
    for (auto& k : keep) overlap |= c.i0 <= k.i1 && k.i0 <= c.i1 && c.j0 <= k.j1 && k.j0 <= c.j1;
    if (!overlap) keep.push_back(c);
  }
  std::sort(keep.begin(), keep.end(), [](const Cand& a, const Cand& b) { return std::tie(a.i0, a.j0) < std::tie(b.i0, b.j0); });
  for (auto& c : keep) {
    const Vec3 a = wall.c + hz * (-half_w + c.i0 * P.cell), b = wall.c + hz * (-half_w + (c.i1 + 1) * P.cell);
    Opening o;
    o.kind = c.kind;
    o.x0 = a.x(), o.z0 = a.z(), o.x1 = b.x(), o.z1 = b.z();
    o.y0 = y_lo + c.j0 * P.cell - ref_floor, o.y1 = y_lo + (c.j1 + 1) * P.cell - ref_floor;
    o.conf = c.conf;
    out.push_back(o);
  }
  return out;
}

std::vector<Opening> OpeningDetector::detect(const LiveMap& map, const SparsePointStore& pts, const PlaneViewsFn& views) const {
  std::vector<Opening> out;
  const Aabb& cb = map.camera_bounds();
  if (!cb.valid() || pts.size() == 0) return out;
  const Vec3 room_side = 0.5 * (cb.lo + cb.hi);
  for (auto& [id, pl] : map.planes()) {
    if (pl.kind != "wall") continue;
    ViewsFn f;
    if (views) f = [&views, &id = id](const Vec3& p) { return views(id, p); };
    auto v = detect_wall(pl, map.has_floor(), map.floor_y(), room_side, pts, f);
    out.insert(out.end(), v.begin(), v.end());
  }
  return out;
}

}  // namespace capturekit::guide
