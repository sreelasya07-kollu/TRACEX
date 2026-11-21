#pragma once
// LandmarkLabeler (Phase 9, stage 7): templated, human names for gaps. No LLM, no free text: every phrase is built
// from a small fixed vocabulary and is <= 8 words (spoken as the "why" of a gap, e.g. "behind the sofa").
//   wall / weak gaps: "the window wall" / "the door wall" (an opening sits on it), else "the wall on your left"
//   corners:          "the corner near the door", else "the corner ahead"
//   occluded:         "behind the sofa" | "beside the sofa" | "around the sofa" (no user position)
//   ceiling / floor:  "the ceiling" / "the floor"
// Relative directions (ahead / on your left / on your right / behind you) need the user pose; without it the phrase
// falls back to a neutral one ("an unseen wall", "an unseen corner"). Yaw is about +Y from +Z, positive turns LEFT.
#include <string>
#include <vector>

#include "capturekit/guide/understanding.hpp"

namespace capturekit::guide {

struct LandmarkContext {
  bool has_user = false;
  double ux = 0, uz = 0, yaw = 0;  // user position (x, z) and heading
};

class LandmarkLabeler {
 public:
  /// Spoken name of an object class ("sofa" for couch, "table" for dining_table, ...).
  static const char* class_phrase(ObjectClass c);
  /// "ahead" | "on your left" | "on your right" | "behind you" for the world point (x, z); "" when no user.
  static std::string direction(const LandmarkContext& ctx, double x, double z);
  /// The templated name of `g` (never empty).
  std::string label(const Gap& g, const std::vector<Opening>& openings, const std::vector<Object>& objects,
                    const LandmarkContext& ctx) const;

  double opening_near_m = 1.2;  // an opening this close to the gap centre names it
  double object_near_m = 2.0;   // an object this close to an occluded gap is the landmark
};

}  // namespace capturekit::guide
