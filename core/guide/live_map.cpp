#include "capturekit/guide/live_map.hpp"

#include <algorithm>
#include <cmath>

namespace capturekit::guide {

namespace {
Plane from_record(const bundle::PlaneRecord& r) {
  Plane p;
  p.id = r.id;
  p.kind = p.src_kind = r.kind;
  p.c = {r.center[0], r.center[1], r.center[2]};
  p.n = Vec3(r.normal[0], r.normal[1], r.normal[2]).normalized();
  Vec3 u(r.axis_u[0], r.axis_u[1], r.axis_u[2]);
  u -= p.n * u.dot(p.n);  // make u orthogonal to n
  p.u = u.norm() > 1e-9 ? u.normalized() : p.n.unitOrthogonal();
  p.v = p.n.cross(p.u);
  p.eu = r.extent_u;
  p.ev = r.extent_v;
  p.t_first = p.t_last = r.t_ns;
  return p;
}

/// Gap between the two in-plane rectangles of a and b, measured in a's frame (<= 0 when they overlap).
double rect_gap(const Plane& a, const Plane& b) {
  Vec3 d = b.c - a.c;
  double du = d.dot(a.u), dv = d.dot(a.v);
  // b's extent projected on a's axes (axes parallel up to small rotation)
  double bu = 0.5 * (std::fabs(b.u.dot(a.u)) * b.eu + std::fabs(b.v.dot(a.u)) * b.ev);
  double bv = 0.5 * (std::fabs(b.u.dot(a.v)) * b.eu + std::fabs(b.v.dot(a.v)) * b.ev);
  double gu = std::fabs(du) - 0.5 * a.eu - bu, gv = std::fabs(dv) - 0.5 * a.ev - bv;
  return std::max(gu, gv);
}

void unite(Plane& a, const Plane& b) {
  // Union bounding rectangle in a's frame.
  Vec3 d = b.c - a.c;
  double du = d.dot(a.u), dv = d.dot(a.v);
  double bu = 0.5 * (std::fabs(b.u.dot(a.u)) * b.eu + std::fabs(b.v.dot(a.u)) * b.ev);
  double bv = 0.5 * (std::fabs(b.u.dot(a.v)) * b.eu + std::fabs(b.v.dot(a.v)) * b.ev);
  double u0 = std::min(-0.5 * a.eu, du - bu), u1 = std::max(0.5 * a.eu, du + bu);
  double v0 = std::min(-0.5 * a.ev, dv - bv), v1 = std::max(0.5 * a.ev, dv + bv);
  a.c += a.u * (0.5 * (u0 + u1)) + a.v * (0.5 * (v0 + v1));
  a.eu = u1 - u0;
  a.ev = v1 - v0;
  a.t_last = std::max(a.t_last, b.t_last);
}
}  // namespace

namespace {
/// Fraction of b's rectangle that lies inside a's rectangle (in a's frame; axes parallel up to a small rotation).
double inside_fraction(const Plane& a, const Plane& b) {
  Vec3 d = b.c - a.c;
  double du = d.dot(a.u), dv = d.dot(a.v);
  double bu = 0.5 * (std::fabs(b.u.dot(a.u)) * b.eu + std::fabs(b.v.dot(a.u)) * b.ev);
  double bv = 0.5 * (std::fabs(b.u.dot(a.v)) * b.eu + std::fabs(b.v.dot(a.v)) * b.ev);
  auto frac = [](double c, double h, double half) {
    if (h <= 0) return 0.0;
    double lo = std::max(c - h, -half), hi = std::min(c + h, half);
    return std::max(0.0, hi - lo) / (2 * h);
  };
  return frac(du, bu, 0.5 * a.eu) * frac(dv, bv, 0.5 * a.ev);
}

/// b's rectangle reaches more than tol beyond a's rectangle on either in-plane axis (a's frame).
bool extends_beyond(const Plane& a, const Plane& b, double tol) {
  Vec3 d = b.c - a.c;
  double du = d.dot(a.u), dv = d.dot(a.v);
  double bu = 0.5 * (std::fabs(b.u.dot(a.u)) * b.eu + std::fabs(b.v.dot(a.u)) * b.ev);
  double bv = 0.5 * (std::fabs(b.u.dot(a.v)) * b.eu + std::fabs(b.v.dot(a.v)) * b.ev);
  return std::fabs(du) + bu > 0.5 * a.eu + tol || std::fabs(dv) + bv > 0.5 * a.ev + tol;
}

constexpr int64_t kSubsumeStaleNs = 2'000'000'000;  // ADR 0026 starting guesses
constexpr double kSubsumeOff = 0.15, kSubsumeInside = 0.7, kSubsumeGrow = 0.05;
}  // namespace

void LiveMap::rebuild(const std::string& key) {
  auto& mem = members_[key];
  Plane& out = planes_[key];
  const int64_t t_first = out.t_first;
  auto own = mem.find(key);
  const Plane* first = own != mem.end() ? &own->second : &mem.begin()->second;
  Plane base = *first;
  for (auto& [rid, m] : mem)
    if (&m != first) unite(base, m);  // platform-id order: deterministic
  base.id = key;
  base.t_first = t_first;
  out = base;
}

void LiveMap::subsume_into(const std::string& key) {
  const Plane& q = planes_.at(key);
  std::vector<std::string> victims;
  for (auto& [id, p] : planes_) {
    if (id == key || p.src_kind != q.src_kind || q.t_last - p.t_last < kSubsumeStaleNs) continue;
    if (p.n.dot(q.n) < std::cos(deg2rad(10)) || std::fabs((p.c - q.c).dot(q.n)) > kSubsumeOff) continue;
    if (inside_fraction(q, p) >= kSubsumeInside) victims.push_back(id);
  }
  for (auto& v : victims) {
    for (auto& [rid, _] : members_[v]) subsumed_[rid] = 1, alias_[rid] = key;
    for (auto& [rid, canon] : alias_)
      if (canon == v) canon = key;
    alias_[v] = key;
    subsumed_[v] = 1;
    members_.erase(v);
    planes_.erase(v);
    ++version_;
  }
}

std::string LiveMap::add_plane(const bundle::PlaneRecord& r) {
  Plane np = from_record(r);
  if (np.eu <= 0 || np.ev <= 0 || !np.n.allFinite() || !np.c.allFinite()) return r.id;  // ignore degenerate
  auto al = alias_.find(r.id);
  std::string key = al != alias_.end() ? al->second : r.id;
  if (subsumed_.count(r.id)) {
    // A subsumed plane's late record inside its subsumer carries nothing new. One that grew beyond it is not dropped:
    // still parallel at the same offset, it becomes a member of the subsumer (united); otherwise it is a plane again.
    auto q = planes_.find(key);
    if (q != planes_.end() && !extends_beyond(q->second, np, kSubsumeGrow)) return key;
    subsumed_.erase(r.id);
    if (q == planes_.end() || q->second.src_kind != np.src_kind || np.n.dot(q->second.n) < std::cos(deg2rad(10)) ||
        std::fabs((np.c - q->second.c).dot(q->second.n)) > kSubsumeOff) {
      alias_.erase(r.id);
      key = r.id;
    }
  }
  auto it = planes_.find(key);
  if (it != planes_.end()) {  // an update of a known plane: AR planes grow, the newest geometry of each member wins
    members_[key][r.id] = np;
    rebuild(key);
    ++version_;
    subsume_into(key);
    classify_after(key);
    return key;
  }
  // New id: duplicate of an existing plane? (same kind, nearly parallel, same offset, touching)
  for (auto& [id, ex] : planes_) {
    if (ex.src_kind != np.src_kind || ex.n.dot(np.n) < std::cos(deg2rad(10))) continue;
    if (std::fabs((np.c - ex.c).dot(ex.n)) > 0.10) continue;
    if (rect_gap(ex, np) > 0.30) continue;
    const std::string canon = id;  // (ex keeps its kind; reclassified below)
    alias_[r.id] = canon;
    members_[canon][r.id] = np;
    rebuild(canon);
    ++version_;
    subsume_into(canon);
    classify_after(canon);
    return canon;
  }
  planes_[r.id] = np;
  members_[r.id][r.id] = np;
  ++version_;
  subsume_into(r.id);
  classify_after(r.id);
  return r.id;
}

SurfaceContext LiveMap::context() const {
  SurfaceContext c;
  if (cam_.valid()) c.has_cam = true, c.cam_y = cam_.hi.y();
  for (auto& [_, p] : planes_) {  // the floor reference is the lowest big horizontal plane that lies below the cameras
    if (std::fabs(p.n.y()) < SurfaceClassifier::kHorizontalMinNy || p.eu * p.ev < 0.5) continue;
    if (cam_.valid() && p.c.y() > cam_.lo.y() - 0.3) continue;
    if (!c.has_floor_ref || p.c.y() < c.floor_y) c.has_floor_ref = true, c.floor_y = p.c.y();
  }
  return c;
}

void LiveMap::reclassify(const std::string& id) {
  auto it = planes_.find(id);
  if (it == planes_.end()) return;
  Plane& p = it->second;
  PlaneGeom g;
  g.c = p.c, g.n = p.n, g.area = p.eu * p.ev;
  SurfaceLabel l = cls_.update(id, g, ctx_);
  if (l.kind != p.kind) ++version_;  // the kind feeds coverage
  p.kind = l.kind, p.kind_conf = l.confidence;
}

void LiveMap::classify_after(const std::string& key) {
  if (classify_) {
    const bool had = ctx_valid_;
    const SurfaceContext prev = ctx_;
    ctx_ = context(), ctx_valid_ = true;
    reclassify(key);
    // A moved floor reference changes every other plane's height: give each one hysteresis tick.
    if (had && (ctx_.has_floor_ref != prev.has_floor_ref || ctx_.floor_y != prev.floor_y))
      for (auto& [id, _] : planes_)
        if (id != key) reclassify(id);
  }
  refresh_levels();
}

size_t LiveMap::surface_count() const {
  size_t n = 0;
  for (auto& [_, p] : planes_) n += p.surface();
  return n;
}

void LiveMap::refresh_levels() {
  floor_ok_ = ceil_ok_ = false;
  double best_floor = 1e9, best_ceil = -1e9;
  for (auto& [_, p] : planes_) {
    if (std::fabs(p.n.y()) < 0.9 || p.eu * p.ev < 1.0) continue;  // big horizontal planes only
    if (p.kind == "floor" && p.c.y() < best_floor) best_floor = p.c.y(), floor_ok_ = true;
    if (p.kind == "ceiling" && p.c.y() > best_ceil) best_ceil = p.c.y(), ceil_ok_ = true;
  }
  floor_y_ = floor_ok_ ? best_floor : (cam_.valid() ? cam_.lo.y() - 1.4 : 0.0);
  ceil_y_ = ceil_ok_ ? best_ceil : floor_y_ + 2.5;
}

Aabb LiveMap::extents() const {
  Aabb b;
  for (auto& [_, p] : planes_) {
    if (!p.surface()) continue;
    for (int su : {-1, 1})
      for (int sv : {-1, 1}) b.add(p.c + p.u * (0.5 * su * p.eu) + p.v * (0.5 * sv * p.ev));
  }
  return b;
}

}  // namespace capturekit::guide
