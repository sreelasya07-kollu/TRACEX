#include "capturekit/guide/recovery.hpp"

#include <algorithm>
#include <cmath>

namespace capturekit::guide {

namespace {
constexpr double kNsPerS = 1e9;
double secs(int64_t dt_ns) { return double(dt_ns) / kNsPerS; }
}  // namespace

void TrackingRecovery::prune(int64_t t) {
  const auto window = int64_t(p_.gyro_window_s * kNsPerS);
  while (!gyro_.empty() && t - gyro_.front().t_ns > window) gyro_.pop_front();
}

void TrackingRecovery::on_gyro(int64_t t, double wx, double wy, double wz) {
  const double mag = std::sqrt(wx * wx + wy * wy + wz * wz);
  if (!std::isfinite(mag)) return;
  if (!gyro_.empty() && t < gyro_.back().t_ns) gyro_.clear();  // time went backwards: start a fresh window
  gyro_.push_back({t, mag});
  prune(t);  // bounded: one window of samples (a few hundred at 200-500 Hz)
}

bool TrackingRecovery::gyro_steady(int64_t t) const {
  const auto window = int64_t(p_.gyro_window_s * kNsPerS);
  double sum = 0, sum2 = 0;
  int n = 0;
  for (auto& g : gyro_) {
    if (t - g.t_ns > window || g.t_ns > t) continue;
    sum += g.mag, sum2 += g.mag * g.mag, ++n;
  }
  if (n < p_.gyro_min_samples) return true;
  const double mean = sum / n;
  const double var = std::max(0.0, sum2 / n - mean * mean);
  return std::sqrt(var) <= p_.gyro_std_max;
}

void TrackingRecovery::on_pose(int64_t t, int state, int reason, const Vec3& pos, double yaw,
                               std::vector<RecoveryEvent>& out) {
  const bool normal = state == 0 || state == 1;
  if (!normal) {
    // Armed only once tracking has been NORMAL: ARCore reports PAUSED while it warms up (often > 2 s); that is the normal
    // start, not a loss, and there is no good pose to recover to or to measure drift from.
    if (!have_good_) return;
    if (degraded_since_ < 0) degraded_since_ = t;
    normal_since_ = -1;  // a relapse while settling restarts the settle wait
    if (!active_ && secs(t - degraded_since_) >= p_.enter_s) {
      active_ = true;
      ++losses_;
      RecoveryEvent e;
      e.kind = RecoveryEvent::Kind::Lost;
      e.t_ns = t, e.tracking_state = state, e.tracking_reason = reason;
      e.degraded_s = secs(t - degraded_since_);
      out.push_back(e);
    }
    return;
  }
  if (!active_) {
    degraded_since_ = -1;  // a short dip (< enter_s) is forgotten
    good_pos_ = pos, good_yaw_ = yaw, have_good_ = true;
    return;
  }
  if (normal_since_ < 0) normal_since_ = t, back_pos_ = pos, back_yaw_ = yaw;
  const double settled = secs(t - normal_since_);
  if (settled < p_.settle_s || (settled < p_.settle_max_s && !gyro_steady(t))) return;
  RecoveryEvent e;
  e.kind = RecoveryEvent::Kind::Reset;
  e.t_ns = t;
  e.lost_s = degraded_since_ >= 0 ? secs(t - degraded_since_) : 0;
  if (have_good_) {
    e.drift_m = (back_pos_ - good_pos_).norm();
    e.drift_deg = std::fabs(rad2deg(wrap_pi(back_yaw_ - good_yaw_)));
  }
  if (!std::isfinite(e.drift_m + e.drift_deg)) e.drift_m = e.drift_deg = 0;
  max_drift_m_ = std::max(max_drift_m_, e.drift_m);
  out.push_back(e);
  ++resets_;
  active_ = false;
  degraded_since_ = normal_since_ = -1;
  good_pos_ = pos, good_yaw_ = yaw, have_good_ = true;  // re-anchored: the next loss is measured from here
}

}  // namespace capturekit::guide
