#pragma once
// Planner (Phase 8, stage 4): frontier / next-best-view. Candidate viewpoints are passable lattice
// cells x headings x pitches. Score = coverage gain / (path + turn + safety cost + 1), subject to an
// overlap constraint (the view must share content with what is already mapped, so tracking holds).
// Hysteresis: a new target must beat the current one by `hysteresis`; the current target is kept
// until reached or obsolete. Rule-based fallbacks: sweep (no map yet), walk toward unseen area, finish.
#include <string>
#include <vector>

#include "capturekit/guide/coverage.hpp"
#include "capturekit/guide/nav_grid.hpp"
#include "capturekit/guide/steer.hpp"

namespace capturekit::guide {

struct UserState {
  Vec3 pos{0, 0, 0};
  double yaw = 0, pitch = 0;
  double speed = 0, yaw_rate = 0;  // m/s, rad/s (smoothed)
};

struct Target {
  bool valid = false, fallback = false;
  double x = 0, z = 0, yaw = 0, pitch = 0;
  double gain = 0, score = 0, gain0 = 0;  // gain0: gain when the target was chosen
  int64_t t_set = 0, at_since = -1;       // at_since: when the user first stood at the target pose
  int id = 0;
  // Gap steering (understanding >= 2): the target is a viewpoint from which a known gap is in line of sight.
  bool gap = false;
  int gap_type = 0;            // GapType
  double gap_x = 0, gap_z = 0;  // the point to look at
  std::string why;             // landmark phrase of the gap ("the door wall")
};

enum class Mode { Sweep, Target, Finish };

/// Facts about the map the planner cannot derive from coverage alone.
struct PlanHints {
  bool need_floor = false, need_ceiling = false;  // no such plane detected yet: ask the user to look there
  std::vector<GapTarget> gaps;                    // steerable gaps (understanding >= 2 and Planner::set_steer only)
};

struct Plan {
  Mode mode = Mode::Sweep;
  Target target;
  double completeness = 0;
  double path_m = 0;  // path length to the target
  bool target_changed = false;
  bool reached = false;
  bool discover = false;  // the target is a look-up/look-down to find a missing floor/ceiling plane
};

class Planner {
 public:
  explicit Planner(const GuideParams& p) : p_(p) {}
  /// Phase 9 step 8: switches on gap steering, the minimum hop and the clearance margin. Never called with the flag
  /// off, so the Phase 8 planning path (and every golden) is untouched.
  void set_steer(const SteerParams& sp) { steer_on_ = true, sp_ = sp; }
  bool steering() const { return steer_on_; }
  int gap_picks() const { return gap_picks_; }
  Plan replan(int64_t t_ns, const UserState& u, const Coverage& cov, NavGrid& nav,
              const geometry::Intrinsics& K, double cam_height, const PlanHints& hints = {});
  const Target& current() const { return cur_; }
  /// The user could not carry out the current target (blocked, ignored): remember it and re-plan.
  void penalize_current();
  int switches() const { return switches_; }
  int reached_count() const { return reached_total_; }
  /// For tests: score a viewpoint with the planner's cost model.
  double score_view(const UserState& u, double x, double z, double yaw, double pitch, const Coverage& cov,
                    const NavGrid& nav, const std::vector<float>& dist, const geometry::Intrinsics& K,
                    double cam_height, double* gain_out = nullptr, double* path_out = nullptr) const;

 private:
  struct Visited {
    double x, z, yaw;
    int n;
  };
  struct GapVisit {
    int type;
    double x, z;
    int n;
  };
  double revisit_factor(double x, double z, double yaw) const;
  int gap_visit_count(int type, double x, double z) const;
  void gap_visited(int type, double x, double z);
  const GapTarget* find_gap(const std::vector<GapTarget>& gaps, int type, double x, double z) const;
  /// Gap viewing score of standing at (x, z) and facing the gap; -1 when not allowed / reachable.
  double gap_score(const UserState& u, double x, double z, const GapTarget& g, const NavGrid& nav,
                   const std::vector<float>& dist, double cam_h, double* path_out = nullptr) const;
  GuideParams p_;
  SteerParams sp_;
  bool steer_on_ = false;
  Target cur_;
  std::vector<Visited> visited_;
  std::vector<GapVisit> gap_visits_;
  int next_id_ = 1, switches_ = 0, reached_total_ = 0, discover_tries_ = 0, gap_picks_ = 0;
};

}  // namespace capturekit::guide
