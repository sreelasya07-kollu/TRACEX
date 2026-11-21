#include "capturekit/guide/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace capturekit::guide {

namespace {
constexpr double kBin = 0.05;  // point bins = the SparsePointStore voxel size

int64_t bin_key(int64_t ix, int64_t iz) { return int64_t((uint64_t(uint32_t(ix)) << 32) | uint64_t(uint32_t(iz))); }
int64_t bin_of(double v) { return int64_t(std::floor(v / kBin)); }
}  // namespace

int layout_wall_num(const std::string& id) {
  if (id.size() < 2 || (id[0] != 'W' && id[0] != 'I')) return 0;
  for (size_t i = 1; i < id.size(); ++i)
    if (id[i] < '0' || id[i] > '9') return 0;
  const int n = std::atoi(id.c_str() + 1);
  return id[0] == 'W' ? n : -n;
}

double LayoutSpan::length() const { return std::hypot(x1 - x0, z1 - z0); }

std::vector<LayoutSpan> limit_spans(const std::vector<LayoutSpan>& spans, size_t cap) {
  if (spans.size() <= cap) return spans;
  std::vector<std::vector<LayoutSpan>> walls;  // consecutive runs of one wall
  for (auto& s : spans) {
    if (walls.empty() || walls.back().front().wall != s.wall) walls.emplace_back();
    walls.back().push_back(s);
  }
  auto used = [&](size_t q) {
    size_t u = 0;
    for (auto& w : walls) u += std::min(w.size(), q);
    return u;
  };
  size_t q = 0;  // the largest equal share that fits (0 only when there are more walls than cap)
  while (used(q + 1) <= cap) ++q;
  size_t left = cap - used(q);
  std::vector<LayoutSpan> out;
  for (auto& w : walls) {
    size_t k = std::min(w.size(), q);
    if (w.size() > q && left > 0) ++k, --left;
    if (k == 0) continue;
    while (w.size() > k) {
      size_t v = 0;  // victim: shortest non-blocked span, else shortest; first on ties
      for (size_t i = 1; i < w.size(); ++i)
        if (std::make_pair(w[i].state == 'b', w[i].length()) < std::make_pair(w[v].state == 'b', w[v].length())) v = i;
      size_t n = v == 0 ? 1 : v + 1 == w.size() ? v - 1 : (w[v - 1].state == 'b' && w[v + 1].state != 'b') ? v + 1 : v - 1;
      if (n < v) w[n].x1 = w[v].x1, w[n].z1 = w[v].z1;
      else w[n].x0 = w[v].x0, w[n].z0 = w[v].z0;
      w.erase(w.begin() + long(v));
      for (size_t i = 1; i < w.size();)  // coalesce neighbours that now share a state
        if (w[i].state == w[i - 1].state) {
          w[i - 1].x1 = w[i].x1, w[i - 1].z1 = w[i].z1;
          w.erase(w.begin() + long(i));
        } else {
          ++i;
        }
    }
    out.insert(out.end(), w.begin(), w.end());
  }
  return out;
}

LayoutIndex::LayoutIndex(const LayoutEvidence& ev, double yaw, double floor_y, double ceiling_h)
    : nav_(ev.nav), c_(std::cos(yaw)), s_(std::sin(yaw)) {
  if (!ev.points) return;
  const double ylo = floor_y + 0.1, yhi = floor_y + std::max(0.5, ceiling_h - 0.1);
  ev.points->for_each([&](const SparsePointStore::Voxel& v) {
    const Vec3 p = v.mean();
    if (!p.allFinite() || p.y() < ylo || p.y() > yhi) return;
    P q{p.x() * c_ + p.z() * s_, -p.x() * s_ + p.z() * c_};
    pts_.push_back(q);
    ++bins_[bin_key(bin_of(q.xp), bin_of(q.zp))];
  });
}

int LayoutIndex::count(int64_t ix, int64_t iz) const {
  auto it = bins_.find(bin_key(ix, iz));
  return it == bins_.end() ? 0 : it->second;
}

int LayoutIndex::points_on_line(int axis, double off, double t0, double t1, double tol) const {
  if (pts_.empty() || !(t1 > t0)) return 0;
  int n = 0;
  for (int64_t o = bin_of(off - tol); o <= bin_of(off + tol); ++o)
    for (int64_t t = bin_of(t0); t <= bin_of(t1 - 1e-9); ++t) n += axis == 0 ? count(o, t) : count(t, o);
  return n;
}

bool LayoutIndex::obstacle_at(int axis, int inward, double off, double t, double d0, double d1) const {
  if (!nav_) return false;
  const double step = std::max(0.05, nav_->cell());
  for (double d = d0; d <= d1 + 1e-9; d += step) {
    const double o = off + inward * d;
    const double xp = axis == 0 ? o : t, zp = axis == 0 ? t : o;
    int ix, iz;
    nav_->to_cell({xp * c_ - zp * s_, 0, xp * s_ + zp * c_}, ix, iz);
    if (nav_->obstacle(ix, iz)) return true;
  }
  return false;
}

bool LayoutIndex::infer_line(int axis, double near, double far, double lo, double hi, int min_pts, double min_spread,
                             double& off) const {
  if (pts_.empty() || !(hi > lo)) return false;
  const double omin = std::min(near, far), omax = std::max(near, far);
  std::vector<P> v;  // (offset, along)
  for (auto& p : pts_) {
    const double o = axis == 0 ? p.xp : p.zp, t = axis == 0 ? p.zp : p.xp;
    if (o >= omin && o <= omax && t >= lo && t <= hi) v.push_back({o, t});
  }
  if (int(v.size()) < min_pts) return false;
  const double bin = 0.1;
  const int nb = int(std::ceil((omax - omin) / bin));
  const int dir = far > near ? -1 : 1;  // walk from far toward near
  for (int i = 0; i < nb; ++i) {
    const double b0 = dir < 0 ? far - (i + 1) * bin : far + i * bin, b1 = b0 + bin;
    int cnt = 0;
    double tmin = 1e18, tmax = -1e18;
    for (auto& p : v)
      if (p.xp >= b0 && p.xp < b1) ++cnt, tmin = std::min(tmin, p.zp), tmax = std::max(tmax, p.zp);
    if (cnt < min_pts || tmax - tmin < min_spread) continue;
    const double mid = 0.5 * (b0 + b1);
    double sum = 0;
    int m = 0;
    for (auto& p : v)
      if (std::abs(p.xp - mid) <= bin) sum += p.xp, ++m;
    off = sum / m;
    return true;
  }
  return false;
}

}  // namespace capturekit::guide
