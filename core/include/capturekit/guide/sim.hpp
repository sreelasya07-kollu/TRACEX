#pragma once
// Guidance simulator (Phase 8): deterministic synthetic rooms + a human-walker model + sensor model.
// Answers "does the planner get a person to good coverage faster than a scripted walk?" without
// phones. Ground truth coverage uses the SAME sufficiency rule as the engine, with true occlusion.
// All randomness comes from guide::Rng (seeded), so reports are byte-identical for equal inputs.
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "capturekit/guide/params.hpp"
#include "capturekit/guide/types.hpp"

namespace capturekit::guide::sim {

struct Box {
  double x0, z0, x1, z1, h;
  int cls = 0;  // ObjectClass the detector oracle reports for this block (0 = not a detectable class)
};

/// A door or window cut into wall `wall` (edge poly[wall] -> poly[wall+1]): u0..u1 along that edge (m from
/// its start), y0..y1 above the floor. The wall has no points there; things behind it show through.
struct OpeningSpec {
  int wall = 0;
  int kind = 0;  // OpeningKind: 0 door, 1 window
  double u0 = 0, u1 = 0, y0 = 0, y1 = 0;
};

struct Room {
  std::string name;
  std::vector<OpeningSpec> openings;                              // Phase 9: doors / windows (empty = none)
  std::vector<std::pair<double, double>> poly;                    // CCW footprint in (x, z)
  std::vector<std::array<double, 4>> rects;                       // disjoint rectangles whose union is the footprint
  double height = 2.5;
  std::vector<Box> boxes;                                         // furniture blocks
  double start_x = 1, start_z = 1, start_yaw = 0;
  /// T4 sim realism (ADR 0029): this room defaults to the "arcore" plane mode (Config::planes "").
  bool arcore_planes = false;
  int textureless_wall = -1;  // arcore mode: this wall (poly edge index) never gets a plane and keeps 35 % of its points
};

/// "box", "L", "furniture", "objects", "door_window" (the box room with a 0.9 x 2.0 m door and a 1.2 x 1.0 m window,
/// sill 0.9 m). With guide.understanding on, the report gains "openings" (what the engine detected).
/// "furnished4" (5 x 4.5 m: sofa, wardrobe, bed against three walls, the fourth wall textureless) and "furnishedL"
/// (the L room with the same three pieces): both default to the "arcore" plane mode.
Room make_room(const std::string& name);

struct Human {
  double speed = 0.9;            // m/s
  double compliance = 0.85;      // probability an instruction is followed
  double delay_min = 1.0, delay_max = 3.0;  // reaction delay (s)
  double yaw_noise_deg = 6;      // error when turning toward a bearing
  double turn_dps = 70, pitch_dps = 40;
  /// Phase 9 understanding scenes only (ADR 0015): after a TiltUp/TiltDown the walker holds dwell_s + 0.5 s, then relaxes
  /// pitch back to level at pitch_dps; scripted/wander pitch sweeps shrink to 6 deg. false = Phase 8 walker
  /// (same code paths and RNG draws: goldens and guide-eval byte-identical). Not serialised into the report.
  bool hold_level = false;
};

struct Config {
  std::string room = "box";
  std::string policy = "planner";  // planner | scripted | wander
  uint64_t seed = 0;
  double max_s = 300;
  bool trace = false;  // add a coarse state trace to the report (debugging)
  double overlay_every_s = 0;  // > 0: add "overlay_samples" (OverlayModel JSON every N s of sim time) and "overlay_max_bytes"; 0 leaves the report unchanged
  /// Phase 9 understanding scenes only (ADR 0015): view-driven point model. Each 5 Hz tick casts 120 rays through a
  /// jittered 12x10 pixel grid; the nearest hit (walls with opening cells skipped, floor/ceiling, furniture, through
  /// faces) is kept with p = 1 up to 1.5 m, linear to 0 at 5 m, noise 0.01 + 0.005 * max(0, range - 2) m per axis
  /// (all hypotheses). false = the Phase 8 area-sampled points (same RNG draws). Not serialised into the report.
  bool view_points = false;
  /// Phase 9 step 3, test harness: a seeded 2D detector oracle (own RNG, never touches the engine or the walker) projects the
  /// true boxes of classed furniture at every keyframe (noisy boxes, misses, false positives), an ObjectLifter lifts them
  /// against the sim's own sparse points and the TRUE shell planes, and the report gains an "oracle" block (lifted objects + truth). Any policy.
  bool detector_oracle = false;
  double oracle_miss = 0.15, oracle_false = 0.08;  // per visible box / per keyframe
  /// Phase 9 step 7, test harness (policy "planner", understanding on): a SHADOW RoomShell + GapFinder + virtual coverage
  /// grids (never read by the engine) run next to the engine; every understanding_every_s of sim time the gaps are scored
  /// against the sim's ground truth (hidden wall / corner / floor behind a classed block / ceiling / floor never seen;
  /// false gap = the true region was well seen) and the report gains an "understanding" block. Not serialised elsewhere.
  bool understanding_eval = false;
  double understanding_every_s = 15;
  /// Plane model (ADR 0029). "" = the room's default; "ideal" = one growing rectangle per true surface (Phase 8);
  /// "arcore" = seeded ARCore-like wall planes: split into fragments at furniture against the wall, long fragments
  /// first detected as two planes that later merge (the subsumed id goes stale), random re-ids (old id stale),
  /// 2-4 deg yaw jitter per plane id, the room's textureless wall without a plane. Own RNG; the report gains
  /// "sim_planes". Wall planes only: floor/ceiling/furniture tops as in "ideal".
  std::string planes;
  Human human;
  GuideParams guide;
};

/// Runs one simulated scan and returns the report JSON (sorted keys, fixed rounding).
std::string simulate(const Config& cfg);

}  // namespace capturekit::guide::sim
