#pragma once
// Tracking recovery (issue #1, ADR 0031): detects a sustained loss of AR tracking quality and decides when the user's
// "turn the phone slowly in a small circle" gesture has recalibrated it. Deterministic: all time comes from the t_ns of
// the inputs (pose and gyro), never a clock, so live sessions and replays take the same decisions.
//
//   NORMAL --(tracking not NORMAL for >= enter_s)--> RECOVERING   emits Lost
//   RECOVERING --(NORMAL for >= settle_s AND gyro steady, or NORMAL for >= settle_max_s)--> NORMAL   emits Reset
//
// "Not NORMAL" is tracking_state 2 (LIMITED; ARCore TrackingState.PAUSED, incl. INSUFFICIENT_FEATURES) or 3 (NOT
// AVAILABLE; ARCore STOPPED). Detection is armed by the first NORMAL pose (the warm-up PAUSED phase is not a loss).
// A relapse while settling restarts the settle wait. Reset carries the drift delta: how far
// the reported pose jumped between the last NORMAL pose before the loss and the first NORMAL pose after it.
#include <cstdint>
#include <deque>
#include <vector>

#include "capturekit/guide/types.hpp"

namespace capturekit::guide {

/// Starting guesses (AGENTS.md: tune on recorded data). Not part of GuideParams (and so not of its hash): see ADR 0031.
struct RecoveryParams {
  double enter_s = 2.0;        // issue #1: PAUSED / INSUFFICIENT_FEATURES for > 2 s
  double settle_s = 1.0;       // NORMAL this long before the prompt is dismissed
  double settle_max_s = 4.0;   // NORMAL this long dismisses it even if the gyro never settles (never trap the user)
  double gyro_window_s = 1.0;  // window for the gyro steadiness check
  double gyro_std_max = 0.35;  // rad/s: std-dev of |omega| in the window. A slow steady circle passes, shaking fails
  int gyro_min_samples = 10;   // fewer samples in the window = no IMU evidence, which never blocks the exit
};

struct RecoveryEvent {
  enum class Kind { Lost, Reset };
  Kind kind = Kind::Lost;
  int64_t t_ns = 0;
  int tracking_state = 0, tracking_reason = 0;  // Lost: the state / reason that started the loss
  double degraded_s = 0;                        // Lost: how long tracking had been degraded when recovery started
  double lost_s = 0;                            // Reset: from the start of the degradation to the reset
  double drift_m = 0, drift_deg = 0;            // Reset: pose jump across the loss (position, |yaw|)
};

class TrackingRecovery {
 public:
  explicit TrackingRecovery(const RecoveryParams& p = {}) : p_(p) {}

  /// One pose (strictly increasing t_ns). state 0 (unspecified) counts as NORMAL. Events are appended to `out`.
  void on_pose(int64_t t_ns, int tracking_state, int tracking_reason, const Vec3& pos, double yaw,
               std::vector<RecoveryEvent>& out);
  /// One gyro sample (rad/s). Out-of-order samples restart the window.
  void on_gyro(int64_t t_ns, double wx, double wy, double wz);

  bool active() const { return active_; }
  int losses() const { return losses_; }
  int resets() const { return resets_; }
  double max_drift_m() const { return max_drift_m_; }
  const RecoveryParams& params() const { return p_; }
  /// Gyro steadiness at t_ns (true without enough samples: absence of IMU data never blocks the exit).
  bool gyro_steady(int64_t t_ns) const;

 private:
  void prune(int64_t t_ns);
  RecoveryParams p_;
  bool active_ = false, have_good_ = false;
  int64_t degraded_since_ = -1, normal_since_ = -1;
  Vec3 good_pos_ = Vec3::Zero(), back_pos_ = Vec3::Zero();
  double good_yaw_ = 0, back_yaw_ = 0;
  int losses_ = 0, resets_ = 0;
  double max_drift_m_ = 0;
  struct GyroSample {
    int64_t t_ns;
    double mag;
  };
  std::deque<GyroSample> gyro_;
};

}  // namespace capturekit::guide
