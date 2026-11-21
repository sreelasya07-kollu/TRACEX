#include "capturekit/guide/nav_grid.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace capturekit::guide {

void NavGrid::to_cell(const Vec3& p, int& ix, int& iz) const {
  ix = int(std::floor(p.x() / c_));
  iz = int(std::floor(p.z() / c_));
}

Vec3 NavGrid::to_world(int ix, int iz) const { return {(ix + 0.5) * c_, floor_y_, (iz + 0.5) * c_}; }

void NavGrid::ensure(int ix, int iz) {
  if (nx_ == 0) {
    x0_ = ix - 8, z0_ = iz - 8, nx_ = 17, nz_ = 17;
    cells_.assign(size_t(nx_) * size_t(nz_), {});
    return;
  }
  if (inside(ix, iz)) return;
  const int pad = 8;
  int a0 = std::min(x0_, ix - pad), b0 = std::min(z0_, iz - pad);
  int a1 = std::max(x0_ + nx_, ix + pad + 1), b1 = std::max(z0_ + nz_, iz + pad + 1);
  std::vector<Cell> nc(size_t(a1 - a0) * size_t(b1 - b0));
  for (int z = 0; z < nz_; ++z)
    for (int x = 0; x < nx_; ++x)
      nc[size_t(z + z0_ - b0) * size_t(a1 - a0) + size_t(x + x0_ - a0)] = cells_[size_t(z) * size_t(nx_) + size_t(x)];
  cells_.swap(nc);
  x0_ = a0, z0_ = b0, nx_ = a1 - a0, nz_ = b1 - b0;
  have_clearance_ = false;
}

void NavGrid::add_point(const Vec3& p) {
  double h = p.y() - floor_y_;
  if (h < p_.obstacle_min_h || h > p_.obstacle_max_h) return;
  int ix, iz;
  to_cell(p, ix, iz);
  ensure(ix, iz);
  Cell& c = cells_[index(ix, iz)];
  if (c.hits < 255) ++c.hits;
  c.max_h = std::max(c.max_h, float(h));
  have_clearance_ = false;
}

bool NavGrid::add_object_box(const Object& o) {
  if (object_class_wall_mounted(o.cls) || !(o.size.x() > 0) || !(o.size.z() > 0)) return false;
  const double top = o.centre.y() + 0.5 * o.size.y() - floor_y_;
  if (top < p_.obstacle_min_h) return false;
  const double ca = std::cos(o.yaw), sa = std::sin(o.yaw);  // depth axis (sin yaw, cos yaw), width axis (cos yaw, -sin yaw)
  const double hw = 0.5 * o.size.x() + 0.5 * c_, hd = 0.5 * o.size.z() + 0.5 * c_;
  const double r = std::hypot(hw, hd);
  int ix0, iz0, ix1, iz1;
  to_cell({o.centre.x() - r, 0, o.centre.z() - r}, ix0, iz0);
  to_cell({o.centre.x() + r, 0, o.centre.z() + r}, ix1, iz1);
  ensure(ix0, iz0);
  ensure(ix1, iz1);
  bool any = false;
  for (int iz = iz0; iz <= iz1; ++iz)
    for (int ix = ix0; ix <= ix1; ++ix) {
      const double dx = (ix + 0.5) * c_ - o.centre.x(), dz = (iz + 0.5) * c_ - o.centre.z();
      if (std::fabs(dx * ca - dz * sa) > hw || std::fabs(dx * sa + dz * ca) > hd) continue;
      Cell& c = cells_[index(ix, iz)];
      c.obj_h = std::max(c.obj_h, float(std::max(top, 1e-3)));  // object layer: separate from point hits so it can be re-stamped
      any = true;
    }
  have_clearance_ = false;
  return any;
}

void NavGrid::clear_objects() {
  bool any = false;
  for (auto& c : cells_)
    if (c.obj_h > 0) c.obj_h = 0, any = true;
  if (any) have_clearance_ = false;
}

void NavGrid::mark_traversed(const Vec3& p) {
  int cx, cz;
  to_cell(p, cx, cz);
  const int r = int(std::ceil(p_.traverse_radius_m / c_));
  ensure(cx - r, cz - r);
  ensure(cx + r, cz + r);
  for (int dz = -r; dz <= r; ++dz)
    for (int dx = -r; dx <= r; ++dx)
      if (std::hypot(double(dx), double(dz)) * c_ <= p_.traverse_radius_m) cells_[index(cx + dx, cz + dz)].traversed = true;
  have_clearance_ = false;
}

void NavGrid::mark_open(const Vec3& p) {
  int ix, iz;
  to_cell(p, ix, iz);
  ensure(ix, iz);
  cells_[index(ix, iz)].open = true;
  have_clearance_ = false;
}

void NavGrid::carve(const Vec3& from, const Vec3& to) {
  const double dx = to.x() - from.x(), dz = to.z() - from.z(), len = std::hypot(dx, dz);
  const double stop = len - 0.15, step = c_ * 0.5;
  if (stop <= 0) return;
  for (double s = 0; s <= stop; s += step) {
    int ix = int(std::floor((from.x() + dx * s / len) / c_)), iz = int(std::floor((from.z() + dz * s / len) / c_));
    ensure(ix, iz);
    if (near_hits(ix, iz) >= 0) break;  // a ray at head height passes OVER low furniture: never carve through it
    cells_[index(ix, iz)].open = true;
  }
  have_clearance_ = false;
}

bool NavGrid::obstacle(int ix, int iz) const {
  return inside(ix, iz) && (cells_[index(ix, iz)].hits >= uint8_t(p_.obstacle_hits) || cells_[index(ix, iz)].obj_h > 0);
}

double NavGrid::near_hits(int ix, int iz) const {
  double top = -1;
  for (int dz = -1; dz <= 1; ++dz)
    for (int dx = -1; dx <= 1; ++dx)
      if (inside(ix + dx, iz + dz)) {
        const Cell& c = cells_[index(ix + dx, iz + dz)];
        if (c.hits >= 1) top = std::max(top, double(c.max_h));
        if (c.obj_h > 0) top = std::max(top, double(c.obj_h));
      }
  return top;
}

bool NavGrid::known_free(int ix, int iz) const {
  if (!inside(ix, iz)) return false;
  const Cell& c = cells_[index(ix, iz)];
  return (c.traversed || c.open) && c.hits < uint8_t(p_.obstacle_hits) && c.obj_h <= 0;
}

void NavGrid::rebuild() {
  if (have_clearance_ || nx_ == 0) return;
  // Multi-source chamfer distance (metres, capped) from every cell that is NOT known free: obstacles AND
  // unknown space. A target is only passable when its whole body disc is known free.
  const float cap = 3.0f;
  clearance_.assign(cells_.size(), cap);
  using Q = std::pair<float, size_t>;
  std::priority_queue<Q, std::vector<Q>, std::greater<Q>> pq;
  for (int z = 0; z < nz_; ++z)
    for (int x = 0; x < nx_; ++x)
      if (!known_free(x + x0_, z + z0_)) {
        size_t k = size_t(z) * size_t(nx_) + size_t(x);
        clearance_[k] = 0;
        pq.push({0, k});
      }
  while (!pq.empty()) {
    auto [d, k] = pq.top();
    pq.pop();
    if (d > clearance_[k]) continue;
    int x = int(k % size_t(nx_)), z = int(k / size_t(nx_));
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx) {
        if (!dx && !dz) continue;
        int xx = x + dx, zz = z + dz;
        if (xx < 0 || zz < 0 || xx >= nx_ || zz >= nz_) continue;
        float nd = d + float(c_) * ((dx && dz) ? 1.41421356f : 1.0f);
        size_t kk = size_t(zz) * size_t(nx_) + size_t(xx);
        if (nd < clearance_[kk]) clearance_[kk] = nd, pq.push({nd, kk});
      }
  }
  have_clearance_ = true;
}

double NavGrid::clearance(int ix, int iz) const {
  if (!inside(ix, iz) || !have_clearance_) return 3.0;
  return clearance_[index(ix, iz)];
}

bool NavGrid::passable(int ix, int iz) const {
  return known_free(ix, iz) && clearance(ix, iz) >= p_.body_radius_m;
}

bool NavGrid::nearest_passable(int ix, int iz, double max_m, int& ox, int& oz) const {
  const int R = int(std::ceil(max_m / c_));
  double best = 1e18;
  bool found = false;
  for (int dz = -R; dz <= R; ++dz)
    for (int dx = -R; dx <= R; ++dx) {
      double d = std::hypot(double(dx), double(dz));
      if (d * c_ > max_m || d >= best || !passable(ix + dx, iz + dz)) continue;
      best = d, ox = ix + dx, oz = iz + dz, found = true;
    }
  return found;
}

void NavGrid::distances(int sx, int sz, std::vector<float>& dist) const {
  dist.assign(cells_.size(), -1.0f);
  if (!inside(sx, sz) || !passable(sx, sz)) return;
  using Q = std::pair<float, size_t>;
  std::priority_queue<Q, std::vector<Q>, std::greater<Q>> pq;
  dist[index(sx, sz)] = 0;
  pq.push({0, index(sx, sz)});
  while (!pq.empty()) {
    auto [d, k] = pq.top();
    pq.pop();
    if (d > dist[k]) continue;
    int x = int(k % size_t(nx_)) + x0_, z = int(k / size_t(nx_)) + z0_;
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx) {
        if (!dx && !dz) continue;
        int xx = x + dx, zz = z + dz;
        if (!passable(xx, zz)) continue;
        if (dx && dz && (!passable(x + dx, z) || !passable(x, z + dz))) continue;  // no corner cutting
        float nd = d + float(c_) * ((dx && dz) ? 1.41421356f : 1.0f);
        size_t kk = index(xx, zz);
        if (dist[kk] < 0 || nd < dist[kk]) dist[kk] = nd, pq.push({nd, kk});
      }
  }
}

size_t NavGrid::obstacle_cells() const {
  size_t n = 0;
  for (int z = 0; z < nz_; ++z)
    for (int x = 0; x < nx_; ++x) n += obstacle(x + x0_, z + z0_);
  return n;
}

size_t NavGrid::free_cells() const {
  size_t n = 0;
  for (int z = 0; z < nz_; ++z)
    for (int x = 0; x < nx_; ++x) n += known_free(x + x0_, z + z0_);
  return n;
}

size_t NavGrid::passable_cells() const {
  size_t n = 0;
  for (int z = 0; z < nz_; ++z)
    for (int x = 0; x < nx_; ++x) n += passable(x + x0_, z + z0_);
  return n;
}

bool NavGrid::clear(const Vec3& cam, const Vec3& target) const {
  if (nx_ == 0) return true;
  // Walk the XZ segment; an obstacle cell blocks when its top is above the ray height there.
  // The last 0.3 m before the target are ignored: the surface itself (and its 1-cell dilation) produces the
  // obstacle points there, so a wall must not occlude its own lower cells.
  const double dx = target.x() - cam.x(), dz = target.z() - cam.z();
  const double len = std::hypot(dx, dz);
  if (len < 1e-6) return true;
  const double stop = std::max(0.0, len - 0.30);
  const double step = c_ * 0.5;
  for (double s = c_; s < stop; s += step) {
    double t = s / len;
    int ix = int(std::floor((cam.x() + dx * t) / c_)), iz = int(std::floor((cam.z() + dz * t) / c_));
    double top = near_hits(ix, iz);  // dilated: sparse points leave gaps in obstacle faces
    if (top < 0) continue;
    double ray_h = cam.y() + (target.y() - cam.y()) * t - floor_y_;
    if (top >= ray_h - 0.05) return false;
  }
  return true;
}

}  // namespace capturekit::guide
