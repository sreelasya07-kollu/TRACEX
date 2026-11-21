#include "capturekit/governor/governor.hpp"

#include <algorithm>
#include <cmath>

#include "capturekit/util/json.hpp"

namespace capturekit::governor {

const char* level_name(Level l) {
  switch (l) {
    case Level::Nominal: return "NOMINAL";
    case Level::ReduceKeyframes: return "REDUCE_KEYFRAMES";
    case Level::ReduceMl: return "REDUCE_ML";
    case Level::LowerResolution: return "LOWER_RESOLUTION";
    case Level::Warn: return "WARN";
    case Level::SafePause: return "SAFE_PAUSE";
  }
  return "UNKNOWN";
}

int thermal_level_from_name(std::string_view n) {
  if (n == "nominal") return 0;
  if (n == "fair") return 1;
  if (n == "serious") return 2;
  if (n == "critical") return 3;
  return -1;
}

namespace {
template <class F>
void for_each_field(GovernorConfig& c, F&& f) {
  f("frame_budget_ms", c.frame_budget_ms); f("perf_enter1", c.perf_enter1); f("perf_exit1", c.perf_exit1);
  f("perf_enter2", c.perf_enter2); f("perf_exit2", c.perf_exit2); f("drop_enter1", c.drop_enter1);
  f("drop_exit1", c.drop_exit1); f("drop_enter2", c.drop_enter2); f("drop_exit2", c.drop_exit2);
  f("perf_confirm_s", c.perf_confirm_s); f("battery_warn_pct", c.battery_warn_pct);
  f("battery_warn_exit_pct", c.battery_warn_exit_pct); f("battery_pause_pct", c.battery_pause_pct);
  f("battery_pause_exit_pct", c.battery_pause_exit_pct); f("down_dwell_s", c.down_dwell_s);
  f("kf_scale_reduced", c.kf_scale_reduced); f("ml_scale_reduced", c.ml_scale_reduced);
}
}  // namespace

std::optional<GovernorConfig> GovernorConfig::from_json(std::string_view text, std::string* err) {
  auto obj = json::parse_flat(text, err);
  if (!obj) return std::nullopt;
  GovernorConfig c;
  for (auto& [k, v] : *obj) {
    bool found = false;
    for_each_field(c, [&](const char* name, double& field) {
      if (k == name) {
        found = true;
        if (auto* d = std::get_if<double>(&v)) field = *d;
      }
    });
    if (!found) {
      if (err) *err = "unknown governor config key: " + k;
      return std::nullopt;
    }
  }
  return c;
}

std::string GovernorConfig::to_json() const {
  json::Object o;
  auto c = *this;
  for_each_field(c, [&](const char* name, double& field) { o.set(name, field); });
  o.set("version", version);
  return o.dump();
}

Policy policy_for(Level l, const GovernorConfig& c) {
  Policy p;
  p.level = l;
  const int i = int(l);
  p.keyframe_rate_scale = i >= int(Level::ReduceKeyframes) ? c.kf_scale_reduced : 1.0;
  p.ml_rate_scale = i >= int(Level::ReduceMl) ? c.ml_scale_reduced : 1.0;
  p.capture_resolution_tier = i >= int(Level::LowerResolution) ? 1 : 0;
  p.overlay_enabled = i < int(Level::LowerResolution);
  p.upload_allowed = i < int(Level::LowerResolution);  // uploading heats the phone and drains battery
  p.must_pause = l == Level::SafePause;
  if (p.must_pause) p.keyframe_rate_scale = p.ml_rate_scale = 0.0;
  return p;
}

Policy Governor::update(const Signals& s, std::vector<Transition>& out) {
  const auto& c = cfg_;
  const int64_t t = s.t_ns;

  // Thermal: discrete, mapped straight onto the ladder.
  static constexpr int kThermalMap[4] = {0, 1, 3, 5};
  const int thermal = kThermalMap[std::clamp(s.thermal_level, 0, 3)];

  // Performance: hysteresis + persistence before escalating.
  const double ft = c.frame_budget_ms > 0 ? s.frame_time_ms_ema / c.frame_budget_ms : 0;
  const double dr = s.dropped_frame_rate;
  int want = (ft >= c.perf_enter2 || dr >= c.drop_enter2) ? 2 : (ft >= c.perf_enter1 || dr >= c.drop_enter1) ? 1 : 0;
  int stay = (ft >= c.perf_exit2 || dr >= c.drop_exit2) ? 2 : (ft >= c.perf_exit1 || dr >= c.drop_exit1) ? 1 : 0;
  int perf_target = std::max(want, std::min(perf_latch_, stay));
  if (perf_target > perf_latch_) {
    if (!perf_pending_since_) perf_pending_since_ = t;
    if (double(t - *perf_pending_since_) / 1e9 >= c.perf_confirm_s) perf_latch_ = perf_target, perf_pending_since_.reset();
  } else {
    perf_pending_since_.reset();
    perf_latch_ = perf_target;
  }

  // Battery: only when not charging.
  int bat = 0;
  if (!s.charging) {
    int enter = s.battery_pct <= c.battery_pause_pct ? 5 : s.battery_pct <= c.battery_warn_pct ? 4 : 0;
    int keep = s.battery_pct < c.battery_pause_exit_pct ? 5 : s.battery_pct < c.battery_warn_exit_pct ? 4 : 0;
    bat = std::max(enter, std::min(battery_latch_, keep));
  }
  battery_latch_ = bat;

  int target = std::max({thermal, perf_latch_, bat, s.low_power_mode ? 1 : 0});
  std::string why;
  if (target == thermal && thermal > 0) why = "thermal=" + std::to_string(s.thermal_level);
  else if (target == bat && bat > 0) why = "battery=" + std::to_string(int(std::lround(s.battery_pct))) + "%";
  else if (target == perf_latch_ && perf_latch_ > 0) why = dr >= c.drop_exit1 ? "dropped_frames" : "frame_time";
  else if (target > 0) why = "low_power";

  if (!started_) {
    started_ = true;
    last_transition_ns_ = t;
  }
  auto go = [&](Level to, const std::string& reason) {
    out.push_back({t, level_, to, reason});
    level_ = to;
    last_transition_ns_ = t;
    last_reason_ = reason;
  };
  const int64_t dwell = int64_t(c.down_dwell_s * 1e9);
  if (target > int(level_)) {
    go(Level(target), why);
    below_since_.reset();
  } else if (target < int(level_)) {
    if (!below_since_) below_since_ = t;
    if (t - *below_since_ >= dwell && t - last_transition_ns_ >= dwell) {
      go(Level(int(level_) - 1), "recovered");
      below_since_ = t;
    }
  } else {
    below_since_.reset();
  }

  Policy p = policy_for(level_, c);
  if (!s.charging && s.battery_pct <= c.battery_warn_pct) p.upload_allowed = false;
  p.reason = level_ == Level::Nominal ? "nominal" : last_reason_;
  return p;
}

}  // namespace capturekit::governor
