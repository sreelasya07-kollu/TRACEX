#pragma once
// GuideMind parameters: every threshold in one versioned struct (recorded in the bundle manifest by
// hash). Values are starting guesses (AGENTS.md): sweep them with `ckit guide-eval`.
#include <optional>
#include <string>
#include <string_view>

namespace capturekit::guide {

struct GuideParams {
  std::string version = "gp2";  // gp2 (ADR 0026): room layout model at every understanding level
  // Coverage: a cell is sufficient with >= min_views qualifying keyframe views, ray spread >= min_spread_deg.
  double cell_m = 0.10, min_views = 3, min_spread_deg = 15;
  double range_min_m = 0.4, range_max_m = 4.0, max_incidence_deg = 75, min_sharp_norm = 0.5;
  double view_margin_px = 8, complete_threshold = 0.90, use_occlusion = 1;
  // NavGrid
  double nav_cell_m = 0.10, body_radius_m = 0.35, obstacle_min_h = 0.2, obstacle_max_h = 1.8;
  double obstacle_hits = 2, traverse_radius_m = 0.25, camera_height_default = 1.4;
  // Planner
  double lattice_m = 0.5, headings = 8, pitch_up_deg = 25, replan_hz = 2, hysteresis = 1.3;
  double reach_m = 0.5, reach_yaw_deg = 30, dwell_s = 3, realized_gain = 0.5, min_gain = 4, overlap_min = 0.2;
  double instr_overhead_m = 3.0, turn_cost_m_per_rad = 0.5, safety_margin_m = 0.6, safety_penalty_m = 1.0, cell_stride = 3;
  double revisit_decay = 0.5, sweep_after_s = 3, discover_after = 0.5, discover_tries = 3, discover_pitch_deg = 35;
  // Instructions
  double min_gap_s = 2, cd_slow_down = 4, cd_hold_still = 3, cd_move = 6, cd_rotate = 5, cd_tilt = 5;
  double cd_step_back = 8, cd_finish = 10, cd_light = 15, cd_sweep = 12, expire_s = 12;
  double blur_frames = 3, lost_relocalize_s = 5, tilt_deg = 15, rotate_deg = 40;
  // Phase 9 room understanding: 0 = off (Phase 8 behaviour, byte-identical); omitted from to_json() while 0 so
  // the config hash of every existing bundle is unchanged.
  double understanding = 0;

  static std::optional<GuideParams> from_json(std::string_view flat_json, std::string* err = nullptr);
  std::string to_json() const;  // canonical, sorted keys
  std::string hash() const;     // first 16 hex chars of sha256(to_json())
};

}  // namespace capturekit::guide
