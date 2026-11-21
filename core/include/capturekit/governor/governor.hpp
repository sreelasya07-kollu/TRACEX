#pragma once
// Thermal and battery governor (Phase 5): pure C++ state machine, time only from Signals.t_ns.
//
// Ladder: Nominal -> ReduceKeyframes -> ReduceMl -> LowerResolution -> Warn -> SafePause.
// Escalation is immediate (performance signals must persist perf_confirm_s first); de-escalation goes
// one level at a time after down_dwell_s below the current level. Continuous signals use separate
// enter/exit thresholds. Every transition is reported (the session writes it to /events).
//
// Safe-pause contract: when Policy.must_pause becomes true the session stops ingesting frames,
// flushes the stream writer and checkpoints (Phase 6); it resumes only after the governor leaves
// SafePause and the user confirms.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace capturekit::governor {

enum class Level : int { Nominal = 0, ReduceKeyframes, ReduceMl, LowerResolution, Warn, SafePause };
const char* level_name(Level l);

/// iOS ProcessInfo.ThermalState names: nominal=0, fair=1, serious=2, critical=3; -1 if unknown.
int thermal_level_from_name(std::string_view name);

struct Signals {
  int64_t t_ns = 0;
  int thermal_level = 0;  // 0..3 (platform adapters map OS states onto this)
  bool low_power_mode = false;
  double battery_pct = 100;
  bool charging = false;
  double frame_time_ms_ema = 0;   // self-measured per-frame processing time
  double dropped_frame_rate = 0;  // fraction of frames dropped recently
};

struct Policy {
  Level level = Level::Nominal;
  double keyframe_rate_scale = 1.0;
  double ml_rate_scale = 1.0;
  int capture_resolution_tier = 0;  // 0 = full, 1 = reduced
  bool overlay_enabled = true;
  bool upload_allowed = true;
  bool must_pause = false;
  std::string reason;
};

struct Transition {
  int64_t t_ns = 0;
  Level from = Level::Nominal, to = Level::Nominal;
  std::string reason;
};

struct GovernorConfig {
  std::string version = "g1";
  double frame_budget_ms = 33.3;
  double perf_enter1 = 1.25, perf_exit1 = 1.10;  // frame_time / budget
  double perf_enter2 = 1.60, perf_exit2 = 1.35;
  double drop_enter1 = 0.10, drop_exit1 = 0.05;
  double drop_enter2 = 0.25, drop_exit2 = 0.15;
  double perf_confirm_s = 1.5;
  double battery_warn_pct = 15, battery_warn_exit_pct = 18;
  double battery_pause_pct = 5, battery_pause_exit_pct = 8;
  double down_dwell_s = 10;
  double kf_scale_reduced = 0.5;
  double ml_scale_reduced = 0.25;

  static std::optional<GovernorConfig> from_json(std::string_view flat_json, std::string* err = nullptr);
  std::string to_json() const;
};

/// Policy for a ladder level (before signal-specific adjustments).
Policy policy_for(Level l, const GovernorConfig& cfg);

class Governor {
 public:
  explicit Governor(const GovernorConfig& cfg = {}) : cfg_(cfg) {}
  /// Feeds signals (t_ns non-decreasing); appends any transition to `out`; returns current policy.
  Policy update(const Signals& s, std::vector<Transition>& out);
  Level level() const { return level_; }

 private:
  GovernorConfig cfg_;
  Level level_ = Level::Nominal;
  int perf_latch_ = 0, battery_latch_ = 0;
  std::optional<int64_t> perf_pending_since_, below_since_;
  int64_t last_transition_ns_ = 0;
  bool started_ = false;
  std::string last_reason_ = "nominal";
};

}  // namespace capturekit::governor
