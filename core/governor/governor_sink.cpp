#include "capturekit/governor/governor_sink.hpp"

#include <cmath>
#include <stdexcept>

#include "capturekit/util/json.hpp"

namespace capturekit::governor {

std::unique_ptr<replay::ReplaySink> GovernorSink::create(const std::string& cfg_json) {
  std::string err;
  auto cfg = GovernorConfig::from_json(cfg_json, &err);
  if (!cfg) throw std::invalid_argument(err);
  return std::make_unique<GovernorSink>(*cfg);
}

namespace {
const std::string* attr(const bundle::EventRecord& e, const char* k) {
  for (auto& [a, v] : e.attrs)
    if (a == k) return &v;
  return nullptr;
}
double num(const std::string* s, double def) {
  if (!s) return def;
  try {
    return std::stod(*s);
  } catch (...) {
    return def;
  }
}
}  // namespace

void GovernorSink::step(int64_t t) {
  if (!started_) {
    started_ = true;
    t0_ = t_last_ = t;
  }
  if (t < t_last_) return;  // never move time backwards (jittered input)
  time_in_[int(cur_)] += t - t_last_;
  t_last_ = t;
  sig_.t_ns = t;
  last_ = gov_.update(sig_, tr_);
  cur_ = last_.level;
}

void GovernorSink::on_event(const bundle::EventRecord& e) {
  if (e.source != "platform") return step(e.t_ns);
  if (e.code == "thermal") {
    if (auto* s = attr(e, "state")) {
      int l = thermal_level_from_name(*s);
      if (l >= 0) sig_.thermal_level = l;
    }
  } else if (e.code == "power") {
    sig_.battery_pct = num(attr(e, "battery_pct"), sig_.battery_pct);
    sig_.charging = num(attr(e, "charging"), sig_.charging) != 0;
    sig_.low_power_mode = num(attr(e, "low_power"), sig_.low_power_mode) != 0;
  } else if (e.code == "perf") {
    sig_.frame_time_ms_ema = num(attr(e, "frame_time_ms"), sig_.frame_time_ms_ema);
    sig_.dropped_frame_rate = num(attr(e, "dropped_rate"), sig_.dropped_frame_rate);
  }
  step(e.t_ns);
}

void GovernorSink::on_end(int64_t t) { step(t); }

std::string GovernorSink::report_json() const {
  json::Object o;
  std::vector<std::string> tr;
  int max_level = 0, pauses = 0;
  for (auto& x : tr_) {
    json::Object to;
    to.set("t_s", std::round(double(x.t_ns - t0_) / 1e6) / 1e3)
        .set("from", level_name(x.from)).set("to", level_name(x.to)).set("reason", x.reason);
    tr.push_back(to.dump());
    max_level = std::max(max_level, int(x.to));
    pauses += x.to == Level::SafePause;
  }
  o.raw("transitions", json::array(tr.begin(), tr.end()));
  json::Object tin;
  for (auto& [l, ns] : time_in_) tin.set(level_name(Level(l)), std::round(double(ns) / 1e6) / 1e3);
  o.set("time_in_level_s", tin);
  o.set("max_level", level_name(Level(max_level))).set("safe_pauses", pauses);
  json::Object p;
  p.set("level", level_name(last_.level)).set("keyframe_rate_scale", last_.keyframe_rate_scale)
      .set("ml_rate_scale", last_.ml_rate_scale).set("capture_resolution_tier", last_.capture_resolution_tier)
      .set("overlay_enabled", last_.overlay_enabled).set("upload_allowed", last_.upload_allowed)
      .set("must_pause", last_.must_pause);
  o.set("final_policy", p);
  o.raw("config", cfg_.to_json());
  return o.dump();
}

}  // namespace capturekit::governor
