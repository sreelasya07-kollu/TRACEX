#pragma once
// OpeningDetector (Phase 9, stage 2): doors and windows from geometry only. Per wall plane a 10 cm raster
// (along the wall x height above the floor) counts sparse points ON the plane (solid) and points seen
// THROUGH it (behind the wall, on the far side from the cameras). Single empty cells are closed when most
// neighbours are solid (sparse sampling). Candidates are MAXIMAL empty rectangles bounded by wall on the
// sides and top (a region that touches the edge of the observed wall is unknown, not an opening):
//   door   = touches the floor, 0.7-1.2 m wide, >= 1.9 m high
//   window = sill 0.6-1.2 m above the floor, 0.4-2.5 m wide, >= 0.4 m high
// "Empty" is ambiguous with sparse points: never-viewed wall looks the same as a hole. An optional `views`
// function (Coverage::views_at) separates them: a gap cell is free space only with >= min_cell_views views, else
// unknown (never opening evidence); a candidate needs most of its cells and of its known surround in view. Confidence = solid surround, seen fraction, through-points. Overlapping
// candidates keep the most confident. Pure and deterministic. Thresholds are synthetic starting guesses
// (Gate 0, docs/STATUS.md section 6).
#include <functional>
#include <string>
#include <vector>

#include "capturekit/guide/live_map.hpp"
#include "capturekit/guide/point_store.hpp"
#include "capturekit/guide/understanding.hpp"

namespace capturekit::guide {

struct OpeningParams {
  double cell = 0.10;            // raster cell (m)
  double on_plane_tol = 0.04;    // |distance to plane| for a solid hit
  double floor_band = 0.04;      // points this close above floor_y are floor (they would fill a door's bottom row)
  double through_min = 0.15, through_max = 1.5;  // behind-the-wall band for through-points
  double floor_snap = 0.3;       // wall bottom within this of floor_y: the raster starts at the floor
  int close_min_neighbours = 5;  // an empty cell with >= this many solid 8-neighbours counts as solid
  double door_min_w = 0.7, door_max_w = 1.2, door_min_h = 1.9;
  double win_sill_min = 0.6, win_sill_max = 1.2, win_min_w = 0.4, win_max_w = 2.5, win_min_h = 0.4;
  int min_cell_views = 3;        // views (Coverage::views_at) for an empty cell to count as free space (hypothesis)
  double min_seen = 0.6;         // fraction of the candidate's cells that were in view
  double min_ring_known = 0.5;   // fraction of the one-cell ring that is solid or in view
  double min_ring_fill = 0.6;    // solid fraction of the known ring cells
  double min_conf = 0.7;         // sim: false windows scored 0.52-0.65, true 0.76-0.94 (ADR 0015; hypothesis)
  double min_wall_w = 1.0, min_wall_h = 1.5;  // smaller wall planes are not rastered
};

class OpeningDetector {
 public:
  /// World point on the wall -> qualifying views of its coverage cell. Empty function = everything counts as viewed.
  using ViewsFn = std::function<int(const Vec3&)>;
  using PlaneViewsFn = std::function<int(const std::string& plane_id, const Vec3&)>;

  explicit OpeningDetector(const OpeningParams& p = {}) : p_(p) {}
  const OpeningParams& params() const { return p_; }

  /// Openings of one wall plane. The platform normal faces the room; it is flipped only when `room_side` (the
  /// camera centre) lies clearly (> 0.5 m) behind it. Points beyond the wall count as "through".
  std::vector<Opening> detect_wall(const Plane& wall, bool has_floor, double floor_y, const Vec3& room_side,
                                   const SparsePointStore& pts, const ViewsFn& views = {}) const;
  /// All wall planes of the map (kind == "wall"), in plane-id order. Opening::wall stays empty (the shell
  /// attaches stable ids later).
  std::vector<Opening> detect(const LiveMap& map, const SparsePointStore& pts, const PlaneViewsFn& views = {}) const;

 private:
  OpeningParams p_;
};

}  // namespace capturekit::guide
