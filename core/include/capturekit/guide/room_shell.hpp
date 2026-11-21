#pragma once
// RoomShell (Phase 9, stage 6): the live room outline from wall planes, deterministic and pure.
//  1. Manhattan yaw = area-weighted circular mean of the wall normals mod 90 deg (4*theta doubling); walls within
//     snap_deg of an axis snap to it, the others are ignored (and mark the shell low-confidence when large).
//  2. Collinear snapped walls (same axis, offset within merge_off, gap <= merge_gap) merge into one wall line.
//  3. Corners = intersections of adjacent wall lines. When the walls form one closed loop of 4 (rectangle) or 6
//     (L) lines the polygon is exact; otherwise the Manhattan bounding rectangle is used, sides without an
//     observed wall become inferred walls, and the shell is flagged low_confidence.
//  4. Wall ids (ADR 0022 scheme): "W<n>" with n = the 1-based first-seen wall number GuideEngine::assign_wall_ids
//     gives a wall plane, never reused; a merged wall line takes the smallest n of its planes, so ids do not churn
//     when planes grow. A wall line takes the SMALLEST first-seen number of its planes, so it can be renamed when a
//     lower-numbered plane later joins it. Pass the engine's map, or nullptr and the shell numbers wall planes
//     itself in first-seen order; own numbering (ids == nullptr) must never be mixed with engine ids. Inferred
//     walls are "I<k>" (own namespace, never collides with W ids), tracked by their outward world direction so the
//     id survives a Manhattan-yaw flip at +-45 degrees.
//  5. Ceiling height = ceiling plane - LiveMap::floor_y() (floor plane, else its camera prior) when sane, else
//     floor_y + 2.5 m flagged inferred.
//  6. Openings (OpeningDetector output, wall empty) attach to the nearest observed shell wall within attach_dist.
// Layout model (real-room-fix T1, ADR 0026; runs at every understanding level inside GuideEngine):
//  7. Lines carry their inward side (k): opposite faces never merge. Fragments down to min_frag_w count; a line
//     narrower than min_wall_w is dropped. Collinear fragments also merge across gaps up to bridge_max when at least
//     bridge_explained of the gap is explained (LayoutIndex: obstacles in front, or wall points on the line) and no
//     perpendicular line crosses the gap.
//  8. A side of the bounding outline without a wall line is inferred from sparse points beyond it (outside the
//     camera path), refined by the floor plane edge; such a wall is inferred (no plane) but evidenced.
//  9. Line ids are tracked: a line takes the smallest number among its planes and the previous lines it overlaps
//     (tracks), so an id survives plane re-ids / aliasing and is never given to a different place.
// 10. spans(): every wall cut into observed / blocked / missing stretches (layout.hpp).
// World frame: Y up, metres. Thresholds are synthetic starting guesses (Gate 0, docs/STATUS.md section 6).
#include <map>
#include <string>
#include <vector>

#include "capturekit/guide/layout.hpp"
#include "capturekit/guide/live_map.hpp"
#include "capturekit/guide/understanding.hpp"

namespace capturekit::guide {

struct ShellParams {
  double snap_deg = 8.0;         // wall normal within this of a Manhattan axis snaps to it
  double min_wall_w = 0.6;       // horizontal width (m) of a wall LINE to count
  double min_frag_w = 0.3;       // horizontal width (m) of a wall plane fragment to count
  double merge_off = 0.15;       // same-axis, same-side walls this close (m) in offset are one line (alcoves >= 0.2 stay apart)
  double merge_gap = 1.0;        // ... when their spans are at most this far apart (m)
  double bridge_max = 3.5;       // ... or this far apart when the gap is explained (furniture in front / wall points)
  double bridge_explained = 0.6; // fraction of 10 cm gap samples that must be explained
  double strip_d0 = 0.25, strip_d1 = 1.2;  // obstacle strip in front of a wall (m inward)
  double line_tol = 0.1;         // wall points within this of a line's offset lie on it (m)
  double track_off = 0.3, track_deg = 20.0;  // a line continues a previous line (id) within these
  int infer_min_pts = 8;         // plane-less wall: point voxels in one 10 cm offset bin ...
  double infer_min_spread = 0.5; // ... spread along the wall over at least this (m)
  double infer_in = 1.0, infer_out = 3.0;  // search window inward / outward of the bounding side (m)
  double cam_clear = 0.2;        // an inferred wall lies at least this far outside the camera path (m)
  double floor_edge_tol = 0.3;   // floor plane edge within this of the point line refines it (mean)
  double span_step = 0.1, span_min = 0.3;  // span sampling and minimum span length (m)
  double corner_tol = 0.75;      // a wall end may fall this short of / beyond a corner (m)
  double side_match = 0.5;       // fallback: observed wall within this of a rectangle side is that side
  double min_dim = 1.0;          // fallback rectangle smaller than this on an axis: no polygon
  double attach_dist = 0.3;      // opening midpoint to wall segment (m)
  double default_ceiling = 2.5;  // floor_y + this when no ceiling plane
  double ceiling_min = 1.8, ceiling_max = 6.0;
  double big_wall_w = 1.0;       // an ignored non-Manhattan wall this wide marks the shell low-confidence
};

class RoomShell {
 public:
  using WallIds = std::map<std::string, int>;  // wall plane id -> 1-based wall number (ADR 0022)

  explicit RoomShell(const ShellParams& p = {}) : p_(p) {}
  const ShellParams& params() const { return p_; }

  /// Rebuilds the shell from the map's wall planes. `ids` = the engine's wall numbers (or nullptr). `ev` = points and
  /// obstacles for gap bridging, plane-less walls and spans (nullptr: planes only).
  void update(const LiveMap& map, const WallIds* ids = nullptr, const std::vector<Opening>& openings = {},
              const LayoutEvidence* ev = nullptr);

  const ShellModel& shell() const { return shell_; }
  /// Per wall of shell().walls (same order): number, member planes, evidence.
  const std::vector<LayoutWall>& layout_walls() const { return lwalls_; }
  /// Observed / blocked / missing stretches of every wall, wall by wall in shell order.
  const std::vector<LayoutSpan>& spans() const { return spans_; }
  /// The wall lines of the last update (Manhattan frame), before the outline was closed.
  const std::vector<LayoutLine>& lines() const { return lines_; }
  /// Walls with evidence (observed or inferred from points): the user-facing wall count.
  int evidenced_walls() const;
  /// Closed outline AND every wall evidenced.
  bool closed_with_evidence() const;
  double blocked_m() const;
  /// Openings with `wall` set ("" when no shell wall is within attach_dist).
  const std::vector<Opening>& openings() const { return openings_; }
  double yaw() const { return yaw_; }  // Manhattan yaw (rad): world direction of the shell's x' axis

 private:
  int wall_number(const std::string& plane_id, const WallIds* ids);
  std::string inferred_id(double outward_angle);
  ShellParams p_;
  ShellModel shell_;
  std::vector<Opening> openings_;
  double yaw_ = 0;
  std::map<std::string, int> own_;  // plane id -> number when no engine map is given
  int next_own_ = 1;
  std::vector<std::pair<double, std::string>> inferred_;  // tracks: outward world angle -> "I<k>"
  std::vector<char> inferred_used_;
  int next_inferred_ = 1;
  struct Track {
    double ang = 0;                    // world angle atan2(n.z, n.x) of the inward normal
    double x0 = 0, z0 = 0, x1 = 0, z1 = 0;  // world endpoints of the line when last seen
    int num = 0;
  };
  std::vector<Track> tracks_;  // every W line ever seen, kept (a number is never given to a different place)
  static constexpr int kFreshBase = 100000;  // fresh line numbers (never a plane number) when every candidate is taken; <= OverlayModel::kMaxId
  int next_fresh_ = 1;
  std::vector<LayoutWall> lwalls_;
  std::vector<LayoutSpan> spans_;
  std::vector<LayoutLine> lines_;
};

}  // namespace capturekit::guide
