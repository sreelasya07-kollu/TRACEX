#pragma once
// NavGrid (Phase 8, stage 3): 2D floor occupancy in the gravity-aligned frame.
//   free     = traversed by the camera, or a floor cell that was actually seen ("clearly open")
//   (passable additionally needs the whole body radius around the cell to be known free)
//   obstacle = enough sparse points between obstacle_min_h and obstacle_max_h above the floor
//   unknown  = everything else. Guidance never targets unknown cells.
// Planning uses obstacles inflated by the body radius. Also the line-of-sight oracle for coverage.
#include <cstdint>
#include <vector>

#include "capturekit/guide/coverage.hpp"
#include "capturekit/guide/params.hpp"
#include "capturekit/guide/types.hpp"
#include "capturekit/guide/understanding.hpp"

namespace capturekit::guide {

class NavGrid : public Occluder {
 public:
  explicit NavGrid(const GuideParams& p) : p_(p), c_(p.nav_cell_m) {}

  void set_floor_y(double y) { floor_y_ = y; }
  void add_point(const Vec3& p);
  /// Phase 9: marks the footprint of a lifted object (oriented box standing on the floor) as obstacle, regardless of
  /// points. Cells whose centre is within half a cell of the rotated rectangle are set. Wall-mounted classes (TV)
  /// and objects whose top is below obstacle_min_h are ignored. Returns whether any cell was set.
  bool add_object_box(const Object& o);
  /// Empties the object layer (point obstacles stay). The engine clears and re-stamps all objects on each change, so a
  /// moved or resized object leaves no old cells.
  void clear_objects();
  void mark_traversed(const Vec3& p);  // camera position (disc of traverse_radius_m)
  void mark_open(const Vec3& p);       // a seen floor cell
  /// Free-space carving: the XZ segment camera -> observed point is open (stops 0.15 m short of the point).
  void carve(const Vec3& from, const Vec3& to);

  /// Inflated obstacle distances; call after the map changed and before planning queries.
  void rebuild();

  bool known_free(int ix, int iz) const;
  bool obstacle(int ix, int iz) const;
  bool passable(int ix, int iz) const;  // free and at least body_radius from any obstacle
  /// Distance (m) from the nearest cell that is not known free (obstacle or unknown), capped at 3 m.
  double clearance(int ix, int iz) const;

  void to_cell(const Vec3& p, int& ix, int& iz) const;
  Vec3 to_world(int ix, int iz) const;  // x,z at cell centre, y = floor_y
  /// Nearest passable cell within max_m of (ix,iz); false if none.
  bool nearest_passable(int ix, int iz, double max_m, int& ox, int& oz) const;
  /// Shortest path length (m) over passable cells from the start cell, -1 when unreachable.
  /// dist is a row-major map over bounds(); use index().
  void distances(int sx, int sz, std::vector<float>& dist) const;
  size_t index(int ix, int iz) const { return size_t(iz - z0_) * size_t(nx_) + size_t(ix - x0_); }
  bool inside(int ix, int iz) const { return ix >= x0_ && iz >= z0_ && ix < x0_ + nx_ && iz < z0_ + nz_; }
  int x0() const { return x0_; }
  int z0() const { return z0_; }
  int nx() const { return nx_; }
  int nz() const { return nz_; }
  double cell() const { return c_; }
  double floor_y() const { return floor_y_; }
  size_t obstacle_cells() const;
  size_t free_cells() const;      // known free (traversed or seen floor)
  size_t passable_cells() const;  // free and clear of obstacles by the body radius

  bool clear(const Vec3& cam, const Vec3& target) const override;

 private:
  void ensure(int ix, int iz);
  /// Tallest obstacle point height (m above floor) in the 3x3 neighbourhood of a cell, -1 if none.
  double near_hits(int ix, int iz) const;
  struct Cell {
    uint8_t hits = 0;
    float max_h = 0;
    float obj_h = 0;  // object layer: top height of a lifted object footprint, 0 = none
    bool traversed = false, open = false;
  };
  GuideParams p_;
  double c_, floor_y_ = 0;
  int x0_ = 0, z0_ = 0, nx_ = 0, nz_ = 0;
  std::vector<Cell> cells_;
  std::vector<float> clearance_;  // metres to nearest obstacle
  bool have_clearance_ = false;
};

}  // namespace capturekit::guide
