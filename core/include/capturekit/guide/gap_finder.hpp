#pragma once
// GapFinder (Phase 9, stage 7): "what should be there but was not seen?" = the room shell minus what was observed.
// Pure and deterministic; NO planner effect (step 8 integrates). Gap types and their evidence:
//   MissingWall   inferred shell wall (no wall plane there) whose virtual coverage grid was not looked at
//   UnseenCorner  shell corner (both walls observed) with no wall-plane footprint within corner_unseen
//   UnseenCeiling no ceiling plane and the virtual ceiling grid was not looked at
//   UnseenFloor   no floor plane and the virtual floor grid was not looked at
//   Occluded      floor ring around a lifted object (behind / beside it) with no keyframe view
//   Weak          an observed surface seen (>= weak_seen) but too thinly (completeness < weak_suff)
// "Looked at" = fraction of a virtual grid's cells seen by Coverage::observe (Coverage::sync_virtual), so a gap
// closes when the user pointed the camera there even if nothing was detected (open doorway, glass): hypothesised
// surfaces must not stall completeness. Thresholds are synthetic starting guesses (Gate 0, docs/STATUS.md section 6).
#include <vector>

#include "capturekit/guide/coverage.hpp"
#include "capturekit/guide/landmark.hpp"
#include "capturekit/guide/live_map.hpp"
#include "capturekit/guide/understanding.hpp"

namespace capturekit::guide {

struct GapParams {
  double corner_unseen = 0.45;   // m: corner farther than this from every wall footprint is unseen ...
  double corner_max = 1.5;       // ... but only up to this: a corner no wall heads for is a phantom (open plan, L room), not a gap
  int min_observed_walls = 3;    // MissingWall needs this many observed walls: the inferred side is where the partial extents end
  double looked_frac = 0.5;      // virtual surface with this fraction of cells seen is looked at (no gap)
  double ring_margin = 0.35;     // m beyond the object's half-diagonal where floor samples are taken
  int ring_samples = 12;
  int ring_min_samples = 4;      // fewer in-room samples: no claim
  double ring_unseen_frac = 0.5; // this fraction of unseen floor samples makes an Occluded gap
  size_t weak_min_cells = 25;    // observed plane needs this many valid cells to be judged
  double weak_seen = 0.7, weak_suff = 0.5;
  double pri_wall = 0.9, pri_corner = 0.6, pri_ceiling = 0.35, pri_floor = 0.5, pri_occluded = 0.5, pri_weak = 0.3;
  size_t max_gaps = 6;
};

struct GapInputs {
  const ShellModel* shell = nullptr;
  const LiveMap* map = nullptr;
  const std::vector<Object>* objects = nullptr;    // optional
  const std::vector<Opening>* openings = nullptr;  // optional (names only)
  const Coverage* cov = nullptr;                   // real plane grids (Occluded, Weak need it)
  const Coverage* vcov = nullptr;                  // holds the virtual grids of virtual_surfaces() (optional)
  LandmarkContext user;
};

class GapFinder {
 public:
  explicit GapFinder(const GapParams& p = {}) : p_(p) {}
  const GapParams& params() const { return p_; }

  /// Hypothesised surfaces (inferred walls "v:I<k>", "v:ceiling" when no ceiling plane, "v:floor" when no floor
  /// plane) to give to Coverage::sync_virtual. Empty without a shell polygon.
  static std::vector<VirtualSurface> virtual_surfaces(const ShellModel& shell, const LiveMap& map);

  /// Gaps, highest priority first (ties by type, x0, z0), at most max_gaps; `why` is filled by LandmarkLabeler.
  std::vector<Gap> find(const GapInputs& in) const;

 private:
  GapParams p_;
  LandmarkLabeler labeler_;
};

}  // namespace capturekit::guide
