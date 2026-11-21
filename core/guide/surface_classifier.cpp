#include "capturekit/guide/surface_classifier.hpp"

#include <algorithm>
#include <cmath>

namespace capturekit::guide {

namespace {
double clamp01(double x) { return std::max(0.0, std::min(1.0, x)); }
}  // namespace

SurfaceLabel SurfaceClassifier::classify(const PlaneGeom& p, const SurfaceContext& ctx) {
  SurfaceLabel out;
  const double ny = std::fabs(p.n.y());
  const double area = p.area;
  if (!(area > 0) || !p.n.allFinite() || !p.c.allFinite()) return out;
  if (ny > kHorizontalMinNy) {
    const double align = clamp01((ny - kHorizontalMinNy) / (1.0 - kHorizontalMinNy));  // 1 = perfectly level
    if (!ctx.has_floor_ref && ctx.has_cam && p.c.y() > ctx.cam_y + 0.3) {  // no floor known, plane above the camera
      out.kind = "ceiling", out.confidence = 0.5;
      return out;
    }
    const double ref = ctx.has_floor_ref ? ctx.floor_y : p.c.y();
    const double h = p.c.y() - ref;
    // Margin to the nearest height threshold the label depends on, scaled to 0.5 m for full confidence.
    auto margin = [&](double edge) { return clamp01(std::fabs(h - edge) / 0.5); };
    if (h < kFloorMaxH) {
      out.kind = "floor", out.confidence = std::max(0.3, margin(kFloorMaxH));
    } else if (h > kCeilingMinH) {
      out.kind = "ceiling", out.confidence = std::max(0.3, margin(kCeilingMinH));
    } else if (h <= kTableMaxH) {
      out.kind = "table", out.confidence = std::max(0.3, std::min(margin(kFloorMaxH), margin(kTableMaxH)));
    } else {
      out.kind = "other", out.confidence = std::max(0.3, std::min(margin(kTableMaxH), margin(kCeilingMinH)));
    }
    out.confidence = out.confidence * (0.5 + 0.5 * align) * clamp01(0.4 + area / 2.0);
  } else if (ny < kVerticalMaxNy) {
    const double align = clamp01((kVerticalMaxNy - ny) / kVerticalMaxNy);
    if (area >= kWallMinArea) out.kind = "wall", out.confidence = clamp01(0.4 + area / 4.0) * (0.5 + 0.5 * align);
    else out.kind = "other", out.confidence = clamp01(1.0 - area / kWallMinArea) * (0.5 + 0.5 * align);
  } else {
    out.kind = "other", out.confidence = 0.5;
  }
  out.confidence = std::round(out.confidence * 1e3) / 1e3;
  return out;
}

SurfaceLabel SurfaceClassifier::update(const std::string& id, const PlaneGeom& p, const SurfaceContext& ctx) {
  SurfaceLabel now = classify(p, ctx);
  auto it = state_.find(id);
  if (it == state_.end()) {
    state_[id] = {now, "", 0};
    return now;
  }
  State& s = it->second;
  if (now.kind == s.published.kind) {
    s.published = now;  // same label: take the fresh confidence
    s.pending.clear(), s.streak = 0;
  } else if (now.kind == s.pending) {
    if (++s.streak >= kHysteresis) s.published = now, s.pending.clear(), s.streak = 0;
  } else {
    s.pending = now.kind, s.streak = 1;
    if (kHysteresis <= 1) s.published = now, s.pending.clear(), s.streak = 0;
  }
  return s.published;
}

}  // namespace capturekit::guide
