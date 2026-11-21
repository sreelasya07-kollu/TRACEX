#pragma once
// SurfaceClassifier (Phase 9, stage 1): decides floor / wall / ceiling / table / other for an AR plane from
// its geometry, not from the platform's label (ARCore calls every upward plane "floor"). The platform kind
// stays an input hint only. Pure and deterministic: no clock, no randomness.
//   horizontal (|n.y| > 0.9), h = centre.y - floor_y:
//       h < 0.15 -> floor;  h > 2.0 -> ceiling;  0.15..1.2 -> table;  else other
//   vertical (|n.y| < 0.2): area >= 0.5 m2 -> wall, else other;  anything tilted -> other
// A new label replaces the published one only after it was proposed on 2 consecutive updates.
#include <map>
#include <string>

#include "capturekit/guide/types.hpp"

namespace capturekit::guide {

/// Plane geometry the rules need (world frame, metres). area = full extent_u * extent_v.
struct PlaneGeom {
  Vec3 c = Vec3::Zero(), n = Vec3::UnitY();
  double area = 0;
};

struct SurfaceContext {
  bool has_floor_ref = false;  // false: no horizontal plane known yet (the first plane is taken as the floor)
  double floor_y = 0;
  bool has_cam = false;  // camera positions known: a floor must lie below them, a ceiling above them
  double cam_y = 0;      // highest camera height seen
};

struct SurfaceLabel {
  std::string kind = "other";
  double confidence = 0;  // 0..1, from normal alignment, margin to the height thresholds and area
};

class SurfaceClassifier {
 public:
  static constexpr double kFloorMaxH = 0.15, kTableMaxH = 1.2, kCeilingMinH = 2.0;
  static constexpr double kWallMinArea = 0.5, kHorizontalMinNy = 0.9, kVerticalMaxNy = 0.2;
  static constexpr int kHysteresis = 2;  // consecutive agreeing updates needed to change a label

  /// Instantaneous rule-based label (no memory).
  static SurfaceLabel classify(const PlaneGeom& p, const SurfaceContext& ctx);

  /// One observation of plane `id` with hysteresis. The first observation is published immediately.
  SurfaceLabel update(const std::string& id, const PlaneGeom& p, const SurfaceContext& ctx);
  void forget(const std::string& id) { state_.erase(id); }

 private:
  struct State {
    SurfaceLabel published;
    std::string pending;
    int streak = 0;
  };
  std::map<std::string, State> state_;
};

}  // namespace capturekit::guide
