#include "capturekit/guide/coverage.hpp"

#include <algorithm>
#include <cmath>

namespace capturekit::guide {

namespace {
double angle_deg(const float* a, const float* b) {
  double d = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
  return rad2deg(std::acos(std::clamp(d, -1.0, 1.0)));
}
}  // namespace

void CellStat::add_view(const Vec3& ray, double range, double sharp, int64_t t) {
  float r[3] = {float(ray.x()), float(ray.y()), float(ray.z())};
  if (views == 0) {
    std::copy(r, r + 3, r0);
    std::copy(r, r + 3, r1);
  } else {
    double a0 = angle_deg(r, r0), a1 = angle_deg(r, r1);
    spread_deg = float(std::max({double(spread_deg), a0, a1}));
    if (a0 > d01_deg) d01_deg = float(a0), std::copy(r, r + 3, r1);
  }
  if (views < 255) ++views;
  nearest = std::min(nearest, float(range));
  best_sharp = std::max(best_sharp, float(sharp));
  see(t);
}

ViewCam ViewCam::make(const geometry::PoseWC& T, const geometry::Intrinsics& K) {
  ViewCam c;
  c.pos = T.translation();
  c.R_cw = T.se3().so3().matrix().transpose();
  c.fx = K.fx, c.fy = K.fy, c.cx = K.cx, c.cy = K.cy, c.w = K.width, c.h = K.height;
  return c;
}

VisResult view_cell(const ViewCam& cam, const Vec3& p, const Vec3& n, double margin) {
  VisResult r;
  Vec3 pc = cam.R_cw * (p - cam.pos);
  if (pc.z() <= 0.05) return r;
  double u = cam.fx * pc.x() / pc.z() + cam.cx, v = cam.fy * pc.y() / pc.z() + cam.cy;
  if (u < margin || v < margin || u > cam.w - 1 - margin || v > cam.h - 1 - margin) return r;
  Vec3 d = cam.pos - p;
  r.range = d.norm();
  r.ray = d / std::max(r.range, 1e-9);
  r.cos_inc = std::fabs(n.dot(r.ray));  // planes are two-sided
  r.in_image = true;
  return r;
}

Vec3 Coverage::Grid::center(int i, int j) const { return o + u * ((i0 + i + 0.5)) + v * ((j0 + j + 0.5)); }

Coverage::Grid* Coverage::find(const std::string& id) {
  for (auto& g : grids_)
    if (g.id == id) return &g;
  return nullptr;
}
const Coverage::Grid* Coverage::find(const std::string& id) const {
  for (auto& g : grids_)
    if (g.id == id) return &g;
  return nullptr;
}

// Grid axes are scaled by cell size at construction: u,v here are cell-sized vectors.
void Coverage::resize(Grid& g, double u0, double u1, double v0, double v1) {
  const double c = p_.cell_m;
  int ni0 = int(std::floor(u0 / c)), ni1 = int(std::ceil(u1 / c));
  int nj0 = int(std::floor(v0 / c)), nj1 = int(std::ceil(v1 / c));
  if (g.nx == 0) {
    g.i0 = ni0, g.j0 = nj0;
  }
  int a0 = std::min(g.i0, ni0), b0 = std::min(g.j0, nj0);
  int a1 = std::max(g.i0 + g.nx, ni1), b1 = std::max(g.j0 + g.ny, nj1);
  if (a0 != g.i0 || b0 != g.j0 || a1 - a0 != g.nx || b1 - b0 != g.ny) {
    std::vector<CellStat> nc(size_t(a1 - a0) * size_t(b1 - b0));
    for (int j = 0; j < g.ny; ++j)
      for (int i = 0; i < g.nx; ++i)
        nc[size_t(j + g.j0 - b0) * size_t(a1 - a0) + size_t(i + g.i0 - a0)] = g.cells[size_t(j) * size_t(g.nx) + size_t(i)];
    g.cells.swap(nc);
    g.i0 = a0, g.j0 = b0, g.nx = a1 - a0, g.ny = b1 - b0;
  }
  g.valid.assign(g.cells.size(), 0);
  for (int j = 0; j < g.ny; ++j)
    for (int i = 0; i < g.nx; ++i) {
      double cu = (g.i0 + i + 0.5) * c, cv = (g.j0 + j + 0.5) * c;
      g.valid[size_t(j) * size_t(g.nx) + size_t(i)] = cu >= u0 && cu <= u1 && cv >= v0 && cv <= v1;
    }
}

void Coverage::fit(Grid& g, const Vec3& c, const Vec3& u, const Vec3& v, double eu, double ev) {
  // Rectangle corners in grid axes (unit axes) relative to the grid origin.
  const double cm = p_.cell_m;
  const Vec3 uu = g.u / cm, vv = g.v / cm;
  double u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9;
  for (int su : {-1, 1})
    for (int sv : {-1, 1}) {
      Vec3 q = c + u * (0.5 * su * eu) + v * (0.5 * sv * ev) - g.o;
      u0 = std::min(u0, q.dot(uu)), u1 = std::max(u1, q.dot(uu));
      v0 = std::min(v0, q.dot(vv)), v1 = std::max(v1, q.dot(vv));
    }
  resize(g, u0, u1, v0, v1);
}

// A virtual grid keeps its cells while the surface only grows/shrinks or jitters (resize() refits the extent and keeps
// the statistics of cells that stay); it is rebuilt when an axis turns by more than kVirtualAxisDeg or any corner of
// the new rectangle lies more than half a cell off the grid plane (measured at the extent corners, so a small tilt
// that drifts far from the old plane over a long wall is caught too).
bool Coverage::virtual_axes_stale(const Grid& g, const VirtualSurface& s) const {
  constexpr double kVirtualAxisDeg = 2.0;
  const double cos_tol = std::cos(deg2rad(kVirtualAxisDeg));
  if (g.u.normalized().dot(s.u) < cos_tol || g.v.normalized().dot(s.v) < cos_tol || g.n.normalized().dot(s.n) < cos_tol)
    return true;
  const Vec3 gn = g.n.normalized();
  for (int su : {-1, 1})
    for (int sv : {-1, 1}) {
      const Vec3 q = s.c + s.u * (0.5 * su * s.eu) + s.v * (0.5 * sv * s.ev);
      if (std::fabs((q - g.o).dot(gn)) > 0.5 * p_.cell_m) return true;
    }
  return false;
}

void Coverage::sync_virtual(const std::vector<VirtualSurface>& surfaces) {
  const double c = p_.cell_m;
  grids_.erase(std::remove_if(grids_.begin(), grids_.end(),
                              [&](const Grid& g) {
                                if (!g.virt) return false;
                                for (const auto& s : surfaces)
                                  if (s.id == g.id) return false;
                                return true;
                              }),
               grids_.end());
  for (const auto& s : surfaces) {
    Grid* g = find(s.id);
    if (g && !g->virt) continue;  // never shadow a real plane
    if (!g) {
      Grid ng;
      ng.id = s.id, ng.virt = true;
      ng.o = s.c, ng.n = s.n, ng.u = s.u * c, ng.v = s.v * c;
      grids_.push_back(std::move(ng));
      std::sort(grids_.begin(), grids_.end(), [](const Grid& a, const Grid& b) { return a.id < b.id; });
      g = find(s.id);
    }
    else if (virtual_axes_stale(*g, s)) {
      // Orientation or plane offset moved beyond tolerance: the creation-time axes would put the cells on a plane
      // that is no longer there, and their statistics belong to the old one. Rebuild this grid from scratch.
      Grid ng;
      ng.id = s.id, ng.virt = true;
      ng.o = s.c, ng.n = s.n, ng.u = s.u * c, ng.v = s.v * c;
      *g = std::move(ng);
    }
    fit(*g, s.c, s.u, s.v, s.eu, s.ev);
  }
  ++rev_;
}

void Coverage::sync(const LiveMap& map) {
  const double c = p_.cell_m;
  // A plane re-labelled by the SurfaceClassifier (floor -> table) stops being a coverage target.
  grids_.erase(std::remove_if(grids_.begin(), grids_.end(),
                              [&](const Grid& g) {
                                if (g.virt) return false;
                                auto it = map.planes().find(g.id);
                                return it == map.planes().end() || !it->second.surface();
                              }),
               grids_.end());
  for (auto& [id, pl] : map.planes()) {
    if (!pl.surface()) continue;
    Grid* g = find(id);
    if (!g) {
      Grid ng;
      ng.id = id, ng.o = pl.c, ng.n = pl.n;
      ng.u = pl.u * c, ng.v = pl.v * c;  // cell-sized axes
      grids_.push_back(std::move(ng));
      std::sort(grids_.begin(), grids_.end(), [](const Grid& a, const Grid& b) { return a.id < b.id; });
      g = find(id);
    }
    fit(*g, pl.c, pl.u, pl.v, pl.eu, pl.ev);
  }
  ++rev_;
}

void Coverage::observe(const Obs& o, const Occluder* los, std::vector<Vec3>* seen_floor) {
  ViewCam cam = ViewCam::make(o.pose, o.K);
  const double cos_max = std::cos(deg2rad(p_.max_incidence_deg));
  for (auto& g : grids_) {
    for (int j = 0; j < g.ny; ++j)
      for (int i = 0; i < g.nx; ++i) {
        size_t k = size_t(j) * size_t(g.nx) + size_t(i);
        if (!g.valid[k]) continue;
        Vec3 pc = g.center(i, j);
        VisResult r = view_cell(cam, pc, g.n, p_.view_margin_px);
        if (!r.in_image || r.range > p_.range_max_m * 1.5) continue;
        CellStat& cs = g.cells[k];
        if (p_.use_occlusion > 0 && los && !los->clear(cam.pos, pc)) {
          if (o.keyframe && r.range >= p_.range_min_m && r.range <= p_.range_max_m && cs.occ < 255) ++cs.occ;
          continue;
        }
        bool was_seen = cs.seen;
        bool qualifies = o.keyframe && r.range >= p_.range_min_m && r.range <= p_.range_max_m &&
                         r.cos_inc >= cos_max && o.sharp_norm >= p_.min_sharp_norm;
        if (qualifies) cs.add_view(r.ray, r.range, o.sharp_norm, o.t_ns);
        else cs.see(o.t_ns);
        if (seen_floor && !was_seen && std::fabs(g.n.y()) > 0.9 && g.n.y() * (cam.pos - pc).y() > 0 && pc.y() < cam.pos.y() - 0.5)
          seen_floor->push_back(pc);
      }
  }
  ++rev_;
}

CoverageStats Coverage::stats() const {
  CoverageStats s;
  for (auto& g : grids_)
    for (size_t k = 0; k < g.cells.size() && !g.virt; ++k) {
      if (!g.valid[k] || g.cells[k].unobservable()) continue;
      ++s.valid;
      s.seen += g.cells[k].seen;
      s.sufficient += g.cells[k].sufficient(p_);
    }
  return s;
}

int Coverage::views_at(const std::string& id, const Vec3& p) const {
  const Grid* g = find(id);
  if (!g || g->cells.empty() || g->valid.size() != g->cells.size() || !p.allFinite()) return 0;
  const double c2 = p_.cell_m * p_.cell_m;  // grid axes are cell-sized
  const Vec3 q = p - g->o;
  const double fi = std::floor(q.dot(g->u) / c2), fj = std::floor(q.dot(g->v) / c2);
  if (!std::isfinite(fi) || !std::isfinite(fj)) return 0;
  const int i = int(fi) - g->i0, j = int(fj) - g->j0;
  if (i < 0 || i >= g->nx || j < 0 || j >= g->ny) return 0;
  const size_t k = size_t(j) * size_t(g->nx) + size_t(i);
  return g->valid[k] ? int(g->cells[k].views) : 0;
}

CoverageStats Coverage::stats(const std::string& id) const {
  CoverageStats s;
  if (auto* g = find(id))
    for (size_t k = 0; k < g->cells.size(); ++k) {
      if (!g->valid[k] || g->cells[k].unobservable()) continue;
      ++s.valid;
      s.seen += g->cells[k].seen;
      s.sufficient += g->cells[k].sufficient(p_);
    }
  return s;
}

std::vector<std::string> Coverage::plane_ids() const {
  std::vector<std::string> v;
  for (auto& g : grids_)
    if (!g.virt) v.push_back(g.id);
  return v;
}

void Coverage::rebuild() const {
  if (built_rev_ == rev_) return;
  deficient_.clear();
  all_.clear();
  const int stride = std::max(1, int(p_.cell_stride));
  size_t n = 0;
  for (auto& g : grids_)
    for (int j = 0; j < g.ny && !g.virt; ++j)
      for (int i = 0; i < g.nx; ++i) {
        size_t k = size_t(j) * size_t(g.nx) + size_t(i);
        if (!g.valid[k] || g.cells[k].unobservable()) continue;
        const CellStat& cs = g.cells[k];
        if ((n++ % size_t(stride)) != 0) continue;
        Sample s{g.center(i, j), g.n, 1.0f, cs.seen, cs.views, cs.spread_deg, {cs.r0[0], cs.r0[1], cs.r0[2]}, {cs.r1[0], cs.r1[1], cs.r1[2]}};
        all_.push_back(s);
        if (!cs.sufficient(p_)) deficient_.push_back(s);
      }
  built_rev_ = rev_;
}

const std::vector<Sample>& Coverage::deficient() const {
  rebuild();
  return deficient_;
}

ViewEval Coverage::eval_view(const geometry::PoseWC& T, const geometry::Intrinsics& K, const Occluder* los,
                             bool want_overlap) const {
  rebuild();
  ViewCam cam = ViewCam::make(T, K);
  const double cos_max = std::cos(deg2rad(p_.max_incidence_deg));
  const double stride = std::max(1.0, p_.cell_stride);
  ViewEval e;
  int vis_def = 0, close = 0, far = 0, seen_vis = 0;
  for (auto& s : deficient_) {
    VisResult r = view_cell(cam, s.p, s.n, p_.view_margin_px);
    if (!r.in_image || r.range > p_.range_max_m * 1.5) continue;
    if (los && p_.use_occlusion > 0 && !los->clear(cam.pos, s.p)) continue;
    ++vis_def;
    if (r.range < p_.range_min_m) ++close;
    else if (r.range > p_.range_max_m) ++far;
    else if (r.cos_inc >= cos_max) {
      // Marginal progress: half from reaching min_views, half from ray spread reaching min_spread_deg.
      const double K = std::max(1.0, p_.min_views), S = std::max(1.0, p_.min_spread_deg);
      float ray[3] = {float(r.ray.x()), float(r.ray.y()), float(r.ray.z())};
      double spread2 = s.spread;
      if (s.views > 0) spread2 = std::max({double(s.spread), angle_deg(ray, s.r0), angle_deg(ray, s.r1)});
      double before = 0.5 * std::min(double(s.views), K) / K + 0.5 * std::min(double(s.spread), S) / S;
      double after = 0.5 * std::min(double(s.views) + 1, K) / K + 0.5 * std::min(spread2, S) / S;
      e.gain += (after - before) * double(s.w) * stride;
    }
  }
  if (want_overlap)
    for (auto& s : all_) {
      VisResult r = view_cell(cam, s.p, s.n, p_.view_margin_px);
      if (!r.in_image || r.range > p_.range_max_m * 1.5) continue;
      ++e.visible;
      seen_vis += s.seen;
    }
  e.overlap = e.visible ? double(seen_vis) / e.visible : 0.0;
  if (vis_def) e.too_close = double(close) / vis_def, e.too_far = double(far) / vis_def;
  return e;
}

}  // namespace capturekit::guide
