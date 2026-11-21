#pragma once
// ObjectLifter (Phase 9, stage 3): lifts 2D detections (recorded inputs) to oriented 3D boxes standing on the floor.
//   1. points: sparse points whose projection falls in the detection box, minus wall/floor/ceiling points, kept
//      to the biggest depth cluster -> minimum-area rectangle in XZ + height above the floor
//   2. fallback (too few points): the box's bottom-centre pixel ray hits the floor; class prior size
//   3. tracks: same class and a close footprint centre are one object; published after publish_obs observations
// Pure and deterministic: no clock, no randomness; the order of the detections is the only input order.
// Object::yaw is a rotation about +Y from +Z (box depth axis = (sin yaw, cos yaw)); a box is symmetric, so yaw
// is only meaningful modulo 90 degrees and the lifter reports it in [0, 90).
#include <optional>
#include <vector>

#include "capturekit/guide/point_store.hpp"
#include "capturekit/guide/surface_classifier.hpp"
#include "capturekit/guide/understanding.hpp"

namespace capturekit::guide {

struct LifterParams {
  double min_pts = 8;           // voxels in the frustum cluster needed for a point-based lift
  double surface_tol_m = 0.08;  // points this close to a wall/floor/ceiling plane are not object points
  double floor_tol_m = 0.06;    // ... or this close above the floor
  double max_h = 2.3;           // points above are ceiling / shelves, never part of a floor object
  double depth_gap_m = 0.25;    // a gap in depth this big splits clusters (foreground vs background)
  double box_margin = 0.03;     // detection box grown by this fraction of its size before gathering points
  double merge_m = 0.6;         // track match: same class and centre distance < max(merge_m, 0.5 * track footprint)
  double min_footprint = 0.5;   // a point-based box thinner than this fraction of the class prior's smaller side is a sliver
  double min_score = 0.3;       // detections below are ignored
  double alpha = 0.4;           // exponential blend of a new lift into a track
  int publish_obs = 2;          // observations before an object is published
  int fallback_obs = 3;         // ... and a floor-ray-only track (no point ever confirmed it) needs this many
  size_t max_tracks = 16;
};

/// What the lifter needs of the scene at the detection time (all in the world frame).
struct LiftContext {
  geometry::PoseWC T_world_from_camera;
  geometry::Intrinsics K;
  bool has_floor = false;
  double floor_y = 0;
  std::vector<PlaneGeom> surfaces;  // wall / floor / ceiling planes (only c and n are used)
};

struct Lift {
  Vec3 centre = Vec3::Zero(), size = Vec3::Zero();
  double yaw = 0;
  bool from_points = false;
  int n_pts = 0;
};

class ObjectLifter {
 public:
  explicit ObjectLifter(const LifterParams& p = {}) : p_(p) {}

  /// Class prior footprint / height (width, height, depth) in metres.
  static Vec3 class_prior(ObjectClass c);

  /// Lifts one detection without touching the tracks; nullopt when neither points nor the fallback work.
  std::optional<Lift> lift(const Detection& d, const LiftContext& ctx, const SparsePointStore& pts) const;

  /// lift() + track update. Returns true when a track was created or updated.
  bool on_detection(const Detection& d, const LiftContext& ctx, const SparsePointStore& pts);

  /// Published objects (n_obs >= publish_obs), ordered by id.
  std::vector<Object> objects() const;
  size_t tracks() const { return tracks_.size(); }
  const LifterParams& params() const { return p_; }

 private:
  struct Track {
    Object obj;
    bool from_points = false;
    double score_sum = 0;
    int64_t last_frame = -1;  // frame_id of the last counted observation
  };
  bool published(const Track& t) const;  // the one visibility rule: objects() and eviction both use it
  LifterParams p_;
  std::vector<Track> tracks_;
  uint32_t next_id_ = 1;
};

}  // namespace capturekit::guide
