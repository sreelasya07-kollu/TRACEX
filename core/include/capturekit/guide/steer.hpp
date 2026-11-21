#pragma once
// Phase 9 step 8: gap steering (GuideParams::understanding >= 2 only; value 1 and 0 never read this file's logic).
// Gaps found by GapFinder become planner targets: the user is asked to stand where a gap is in line of sight, a
// moderate distance away, and look at it. A hop minimum and a clearance margin around unseen cells stop the planner
// choosing tiny or unsafe moves (finding of phase9 step 3). All numbers are synthetic starting guesses (Gate 0).
#include <string>

#include "capturekit/guide/understanding.hpp"

namespace capturekit::guide {

struct SteerParams {
  double min_hop_m = 1.0;      // a target is either within reach_m (rotate/tilt in place) or at least this far away
  double unseen_clear_m = 0.6; // a target keeps this distance from every non-known-free cell (obstacle or unseen)
  double gap_gain = 200;       // planner score of a gap view = gap_gain * pri / (path + turn + safety + overhead)
  double gap_dist_min = 1.2, gap_dist_max = 3.5;  // viewing distance band to the gap point
  int max_gap_visits = 2;      // a gap stood at this many times without closing is given up on
  double gap_match_m = 0.6;    // a gap within this of a remembered one (same type) is the same gap
  double gap_after = 0.6;      // gap views steer only once coverage completeness reaches this (or no coverage view is left): first the surfaces that exist, then the ones that are missing (a false gap must not cost coverage time)
};

/// One steerable gap as the planner sees it: a point to look at (x, z, y metres above the floor) and its extent.
struct GapTarget {
  int type = 0;       // GapType
  double x = 0, z = 0, y = 1.0, len = 0, pri = 0;
  std::string why;    // templated landmark phrase (<= 8 words)
};

inline const char* gap_type_name(GapType t) {
  switch (t) {
    case GapType::MissingWall: return "missing_wall";
    case GapType::UnseenCorner: return "unseen_corner";
    case GapType::UnseenCeiling: return "unseen_ceiling";
    case GapType::UnseenFloor: return "unseen_floor";
    case GapType::Occluded: return "occluded";
    case GapType::Weak: return "weak";
  }
  return "unknown";
}

/// Only these gaps steer the planner; ceiling / floor go through the existing discovery looks.
inline bool gap_steers(GapType t) { return t == GapType::MissingWall || t == GapType::UnseenCorner || t == GapType::Occluded; }

}  // namespace capturekit::guide
