#pragma once
// Room layout (real-room-fix T1, ADR 0026): what RoomShell needs beyond the AR planes, and what it outputs per wall.
//  - A wall is a layout LINE: Manhattan axis + inward side + offset cluster, built from one or more plane fragments
//    (RoomShell::update). Its id ("W<n>") follows the line, not a plane.
//  - LayoutIndex holds the extra evidence of one update in the shell's Manhattan frame (x' = x cos + z sin,
//    z' = -x sin + z cos): sparse wall points and the NavGrid obstacle layer. It answers "is this gap explained"
//    (furniture in front / wall points on the line), "where is a plane-less wall" and "is this stretch blocked".
//  - Spans cut every wall into observed / blocked / missing stretches.
// Deterministic and pure: no clock, no randomness, results depend only on the inputs. World frame: Y up, metres.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "capturekit/guide/nav_grid.hpp"
#include "capturekit/guide/point_store.hpp"

namespace capturekit::guide {

/// Evidence beyond the planes; either pointer may be null (then that kind of evidence is simply absent).
struct LayoutEvidence {
  const SparsePointStore* points = nullptr;  // sparse AR points (5 cm voxels)
  const NavGrid* nav = nullptr;              // obstacle layer (points 0.2-1.8 m above the floor, lifted objects)
};

/// One wall of the layout, parallel to ShellModel::walls (same index, same id).
struct LayoutWall {
  std::string id;                   // == ShellWall::id ("W<n>" or "I<k>")
  int num = 0;                      // layout_wall_num(id): n of "W<n>", -k of "I<k>"
  std::vector<std::string> planes;  // member wall planes (empty for a plane-less wall)
  bool evidenced = false;           // observed (planes) or inferred from points; false = a hypothesised side
};

/// A stretch of a layout wall (world metres) and its state: 'o' observed, 'b' blocked, 'm' missing.
struct LayoutSpan {
  std::string wall;
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  char state = 'm';
  double length() const;
};

/// A wall line in the Manhattan frame (debugging / tests): axis 0 is the line x' = off spanning z' in [lo, hi], axis 1
/// is z' = off spanning x'. k = direction of the inward normal (0 +x', 1 +z', 2 -x', 3 -z').
struct LayoutLine {
  int axis = 0, k = 0;
  double off = 0, lo = 0, hi = 0;
  std::string id;
  std::vector<std::string> planes;
  bool from_points = false;  // a plane-less line inferred from sparse points
};

/// "W3" -> 3, "I2" -> -2, anything else -> 0.
int layout_wall_num(const std::string& id);

/// At most `cap` spans (spans grouped wall by wall, as RoomShell::spans()), shared fairly: every wall gets the same
/// quota (leftover to the earlier walls), so later walls are never cut off. A wall over its quota merges its shortest
/// non-blocked span (else its shortest) into a neighbour, so it still covers its full length and blocked spans are
/// the last to go. Deterministic.
std::vector<LayoutSpan> limit_spans(const std::vector<LayoutSpan>& spans, size_t cap);

class LayoutIndex {
 public:
  /// Indexes the points between floor_y + 0.1 and floor_y + ceiling_h - 0.1 in 5 cm bins of the frame of `yaw`.
  LayoutIndex(const LayoutEvidence& ev, double yaw, double floor_y, double ceiling_h);
  bool has_points() const { return !pts_.empty(); }
  bool has_nav() const { return nav_ != nullptr; }
  /// Point voxels within `tol` of the line (axis, off) whose along coordinate is in [t0, t1).
  int points_on_line(int axis, double off, double t0, double t1, double tol) const;
  /// Whether an obstacle cell lies `d0`..`d1` metres inward of the line at along coordinate t. `inward` = +1 when the
  /// room is on the +axis side of the line, -1 otherwise.
  bool obstacle_at(int axis, int inward, double off, double t, double d0, double d1) const;
  /// Plane-less wall search on an axis: scans offsets from `far` back to `near` (outermost first, 10 cm bins) for the
  /// first bin with >= min_pts voxels whose along coordinates in [lo, hi] spread over >= min_spread metres. Returns the
  /// mean offset of the voxels within +-0.1 m of that bin in `off`, or false.
  bool infer_line(int axis, double near, double far, double lo, double hi, int min_pts, double min_spread, double& off) const;

 private:
  struct P {
    double xp, zp;
  };
  const NavGrid* nav_ = nullptr;
  double c_ = 1, s_ = 0;
  std::vector<P> pts_;
  std::unordered_map<int64_t, int> bins_;  // 5 cm bin of (x', z') -> voxel count (count queries only, no iteration)
  int count(int64_t ix, int64_t iz) const;
};

}  // namespace capturekit::guide
