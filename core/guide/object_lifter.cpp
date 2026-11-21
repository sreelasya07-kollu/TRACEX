#include "capturekit/guide/object_lifter.hpp"

#include <algorithm>
#include <cmath>

#include "capturekit/guide/coverage.hpp"

namespace capturekit::guide {

namespace {

constexpr double kHalfPi = kPi / 2;

double norm90(double a) {  // [0, 90 deg)
  a = std::fmod(a, kHalfPi);
  return a < 0 ? a + kHalfPi : a;
}

/// Trimmed extent of a sample (3% off each end once there are enough points to tell an outlier from an edge).
std::pair<double, double> extent(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  if (n < 30) return {v.front(), v.back()};
  return {v[size_t(0.03 * double(n))], v[size_t(std::ceil(0.97 * double(n))) - 1]};
}

}  // namespace

Vec3 ObjectLifter::class_prior(ObjectClass c) {  // (width, height, depth)
  switch (c) {
    case ObjectClass::Chair: return {0.5, 0.9, 0.5};
    case ObjectClass::Couch: return {1.9, 0.85, 0.9};
    case ObjectClass::Bed: return {1.9, 0.55, 1.4};
    case ObjectClass::DiningTable: return {1.4, 0.75, 0.9};
    case ObjectClass::Tv: return {1.0, 0.6, 0.1};
    case ObjectClass::PottedPlant: return {0.4, 0.7, 0.4};
    case ObjectClass::Laptop: return {0.35, 0.03, 0.25};
    case ObjectClass::Bench: return {1.2, 0.45, 0.4};
    case ObjectClass::Toilet: return {0.4, 0.75, 0.65};
    case ObjectClass::Sink: return {0.55, 0.85, 0.45};
    case ObjectClass::Refrigerator: return {0.7, 1.75, 0.7};
    case ObjectClass::Oven: return {0.6, 0.9, 0.6};
    case ObjectClass::Desk: return {1.2, 0.75, 0.6};
    case ObjectClass::Shelf: return {0.8, 1.8, 0.35};
    case ObjectClass::Cabinet: return {0.9, 0.9, 0.5};
    case ObjectClass::Lamp: return {0.35, 1.5, 0.35};
    default: return {0.5, 0.8, 0.5};
  }
}

std::optional<Lift> ObjectLifter::lift(const Detection& d, const LiftContext& ctx, const SparsePointStore& pts) const {
  if (!ctx.has_floor || !(d.x1 > d.x0) || !(d.y1 > d.y0)) return std::nullopt;
  const ViewCam vc = ViewCam::make(ctx.T_world_from_camera, ctx.K);
  const double mx = p_.box_margin * (d.x1 - d.x0), my = p_.box_margin * (d.y1 - d.y0);
  const double bx0 = d.x0 - mx, bx1 = d.x1 + mx, by0 = d.y0 - my, by1 = d.y1 + my;

  // 1. frustum points that are not part of the room shell
  struct P {
    double z;
    Vec3 p;
  };
  std::vector<P> in;
  pts.for_each([&](const SparsePointStore::Voxel& v) {
    const Vec3 p = v.mean();
    const double h = p.y() - ctx.floor_y;
    if (h < p_.floor_tol_m || h > p_.max_h) return;
    const Vec3 q = vc.R_cw * (p - vc.pos);
    if (q.z() < 0.3 || q.z() > 8.0) return;
    const double u = vc.fx * q.x() / q.z() + vc.cx, w = vc.fy * q.y() / q.z() + vc.cy;
    if (u < bx0 || u > bx1 || w < by0 || w > by1) return;
    for (const PlaneGeom& s : ctx.surfaces)
      if (std::fabs(s.n.dot(p - s.c)) < p_.surface_tol_m) return;
    in.push_back({q.z(), p});
  });

  if (double(in.size()) >= p_.min_pts) {
    // keep the biggest depth cluster (ties: the nearer one)
    std::sort(in.begin(), in.end(), [](const P& a, const P& b) {  // voxel position breaks depth ties: order never depends on the hash layout
      if (a.z != b.z) return a.z < b.z;
      if (a.p.x() != b.p.x()) return a.p.x() < b.p.x();
      if (a.p.y() != b.p.y()) return a.p.y() < b.p.y();
      return a.p.z() < b.p.z();
    });
    size_t best_b = 0, best_n = 0, b = 0;
    for (size_t i = 1; i <= in.size(); ++i)
      if (i == in.size() || in[i].z - in[i - 1].z > p_.depth_gap_m) {
        if (i - b > best_n) best_n = i - b, best_b = b;
        b = i;
      }
    if (double(best_n) >= p_.min_pts) {
      std::vector<Vec3> c;
      Vec3 mean = Vec3::Zero();
      for (size_t i = best_b; i < best_b + best_n; ++i) c.push_back(in[i].p), mean += in[i].p;
      mean /= double(c.size());
      // minimum-area rectangle in XZ over yaw 0..85 deg
      double best_area = 1e18, byaw = 0, lx0 = 0, lx1 = 0, lz0 = 0, lz1 = 0;
      std::vector<double> xs(c.size()), zs(c.size());
      for (int k = 0; k < 18; ++k) {
        const double a = deg2rad(5.0 * k), ca = std::cos(a), sa = std::sin(a);
        for (size_t i = 0; i < c.size(); ++i) {
          const double dx = c[i].x() - mean.x(), dz = c[i].z() - mean.z();
          xs[i] = dx * ca - dz * sa, zs[i] = dx * sa + dz * ca;
        }
        auto [a0, a1] = extent(xs);
        auto [c0, c1] = extent(zs);
        const double area = (a1 - a0) * (c1 - c0);
        if (area < best_area) best_area = area, byaw = a, lx0 = a0, lx1 = a1, lz0 = c0, lz1 = c1;
      }
      std::vector<double> hs;
      for (auto& q : c) hs.push_back(q.y() - ctx.floor_y);
      const double top = extent(hs).second;
      const double ca = std::cos(byaw), sa = std::sin(byaw);
      const double mxl = 0.5 * (lx0 + lx1), mzl = 0.5 * (lz0 + lz1);
      Lift L;
      L.centre = {mean.x() + mxl * ca + mzl * sa, ctx.floor_y + 0.5 * top, mean.z() - mxl * sa + mzl * ca};
      L.size = {std::max(0.1, lx1 - lx0), std::max(0.05, top), std::max(0.1, lz1 - lz0)};
      L.yaw = norm90(byaw);
      L.from_points = true, L.n_pts = int(best_n);
      // A sliver (one visible face, or a neighbour's points) is not this class: use the floor-ray fallback instead.
      const Vec3 pr = class_prior(d.cls);
      if (std::min(L.size.x(), L.size.z()) >= p_.min_footprint * std::min(pr.x(), pr.z())) return L;
    }
  }

  // 2. fallback: the bottom-centre pixel ray meets the floor; class prior size
  if (d.y1 >= double(ctx.K.height) - 3) return std::nullopt;  // bottom cut off by the image edge
  const Eigen::Matrix3d R_wc = vc.R_cw.transpose();
  const Vec3 dir = (R_wc * Vec3((0.5 * (d.x0 + d.x1) - vc.cx) / vc.fx, (d.y1 - vc.cy) / vc.fy, 1.0)).normalized();
  if (dir.y() > -0.05) return std::nullopt;
  const double t = (ctx.floor_y - vc.pos.y()) / dir.y();
  if (t < 0.3 || t > 8.0) return std::nullopt;
  const Vec3 hit = vc.pos + dir * t;
  Vec3 hv(dir.x(), 0, dir.z());
  if (hv.norm() < 1e-6) return std::nullopt;
  hv.normalize();
  const Vec3 pr = class_prior(d.cls);
  Lift L;
  L.size = pr;
  L.centre = {hit.x() + hv.x() * 0.5 * pr.z(), ctx.floor_y + 0.5 * pr.y(), hit.z() + hv.z() * 0.5 * pr.z()};
  // The prior's depth runs along the view direction. Reducing the yaw to [0, 90) turns the box by k quarter turns:
  // for odd k its width and depth axes trade places.
  const double raw = std::atan2(hv.x(), hv.z());
  L.yaw = norm90(raw);
  if (long(std::lround((raw - L.yaw) / kHalfPi)) % 2 != 0) std::swap(L.size.x(), L.size.z());
  return L;
}

bool ObjectLifter::on_detection(const Detection& d, const LiftContext& ctx, const SparsePointStore& pts) {
  if (d.cls == ObjectClass::Unknown || d.score < p_.min_score) return false;
  auto L = lift(d, ctx, pts);
  if (!L) return false;

  int bi = -1;
  double bd = 1e18;
  for (size_t i = 0; i < tracks_.size(); ++i) {
    const Object& o = tracks_[i].obj;
    if (o.cls != d.cls) continue;
    const double dist = std::hypot(o.centre.x() - L->centre.x(), o.centre.z() - L->centre.z());
    if (dist < std::max(p_.merge_m, 0.5 * std::max(o.size.x(), o.size.z())) && dist < bd) bd = dist, bi = int(i);
  }
  auto set_from = [&](Track& t, const Lift& l) {
    t.obj.centre = l.centre, t.obj.size = l.size, t.obj.yaw = l.yaw;
    t.from_points = l.from_points;
  };
  if (bi < 0) {
    if (tracks_.size() >= p_.max_tracks) {  // drop the weakest unpublished track
      int ev = -1;
      for (size_t i = 0; i < tracks_.size(); ++i)
        if (!published(tracks_[i]) && (ev < 0 || tracks_[i].score_sum < tracks_[size_t(ev)].score_sum)) ev = int(i);
      if (ev < 0) return false;
      tracks_.erase(tracks_.begin() + ev);
    }
    Track t;
    t.obj.id = next_id_++, t.obj.cls = d.cls, t.obj.n_obs = 1, t.last_frame = int64_t(d.frame_id);
    set_from(t, *L);
    t.score_sum = d.score;
    t.obj.conf = d.score * (t.from_points ? 1.0 : 0.7) / 3.0;
    tracks_.push_back(std::move(t));
    return true;
  }
  Track& t = tracks_[size_t(bi)];
  if (t.last_frame == int64_t(d.frame_id)) return false;  // the same frame never counts twice for one object
  t.last_frame = int64_t(d.frame_id);
  ++t.obj.n_obs;
  t.score_sum += d.score;
  if (L->from_points && !t.from_points) {
    set_from(t, *L);  // a point-based lift replaces a floor-ray guess outright
  } else if (L->from_points == t.from_points) {
    Lift l = *L;
    double dy = l.yaw - t.obj.yaw;
    const double k = std::round(dy / kHalfPi);
    dy -= k * kHalfPi;
    if (int(std::fabs(k)) % 2 == 1) std::swap(l.size.x(), l.size.z());  // the rectangle's axes are swapped
    const double a = p_.alpha;
    t.obj.centre = (1 - a) * t.obj.centre + a * l.centre;
    t.obj.size = (1 - a) * t.obj.size + a * l.size;
    t.obj.yaw = norm90(t.obj.yaw + a * dy);
  }
  t.obj.conf = t.score_sum / t.obj.n_obs * (t.from_points ? 1.0 : 0.7) * std::min(1.0, t.obj.n_obs / 3.0);
  return true;
}

bool ObjectLifter::published(const Track& t) const {
  return t.obj.n_obs >= (t.from_points ? p_.publish_obs : std::max(p_.publish_obs, p_.fallback_obs));
}

std::vector<Object> ObjectLifter::objects() const {
  std::vector<Object> out;
  for (const Track& t : tracks_)
    if (published(t)) out.push_back(t.obj);
  std::sort(out.begin(), out.end(), [](const Object& a, const Object& b) { return a.id < b.id; });
  return out;
}

}  // namespace capturekit::guide
