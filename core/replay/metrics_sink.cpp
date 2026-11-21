#include "capturekit/replay/metrics_sink.hpp"

#include <algorithm>
#include <cmath>

#include "capturekit/util/json.hpp"

namespace capturekit::replay {

bool MetricsSink::accept(const char* name, int64_t t) {
  auto& c = ch_[name];
  if (c.n > 0 && t <= c.last) {
    ++c.rejected;
    return false;
  }
  if (c.n == 0) c.first = t;
  else c.max_gap = std::max(c.max_gap, t - c.last);
  c.last = t;
  ++c.n;
  t_min_ = std::min(t_min_, t);
  t_max_ = std::max(t_max_, t);
  return true;
}

uint64_t MetricsSink::rejected(const std::string& channel) const {
  auto it = ch_.find(channel);
  return it == ch_.end() ? 0 : it->second.rejected;
}

void MetricsSink::on_intrinsics(const bundle::IntrinsicsSample& k) { accept("intrinsics", k.t_ns); }

void MetricsSink::on_pose(const bundle::PoseSample& p) {
  if (!accept("pose", p.t_ns)) return;
  int st = int(p.tracking_state);
  if (!have_pose_) {
    cur_state_ = st;
    cur_start_ = p.t_ns;
  } else if (st != cur_state_) {
    if (cur_state_ != int(bundle::TrackingState::Normal))
      intervals_.push_back({cur_start_, p.t_ns, cur_state_});
    cur_state_ = st;
    cur_start_ = p.t_ns;
  }
  if (have_pose_ && st == int(bundle::TrackingState::Normal)) {
    double d2 = 0;
    for (int i = 0; i < 3; ++i) d2 += std::pow(p.t_world_from_camera[i] - last_p_[i], 2);
    path_m_ += std::sqrt(d2);
  }
  for (int i = 0; i < 3; ++i) last_p_[i] = p.t_world_from_camera[i];
  have_pose_ = true;
}

void MetricsSink::on_gyro(const bundle::ImuSample& s) { accept("gyro", s.t_ns); }
void MetricsSink::on_accel(const bundle::ImuSample& s) { accept("accel", s.t_ns); }

void MetricsSink::on_frame(const FrameView& f) {
  if (accept("frame", f.t_ns) && f.keyframe) ++keyframes_;
}

void MetricsSink::on_event(const bundle::EventRecord& e) {
  ++ch_["event"].n;  // events may share t_ns; no monotonic check
  ++events_[e.source + "." + e.code];
}

void MetricsSink::on_points(const bundle::PointCloudRecord& p) {
  if (accept("points", p.t_ns)) points_ += p.xyz.size() / 3;
}

void MetricsSink::on_plane(const bundle::PlaneRecord& p) { accept("planes", p.t_ns); }

void MetricsSink::on_detections(const bundle::DetectionsRecord& d) { accept("detections", d.t_ns); }

void MetricsSink::on_end(int64_t t) {
  if (ended_) return;
  ended_ = true;
  t_end_ = t;
  if (have_pose_ && cur_state_ != int(bundle::TrackingState::Normal))
    intervals_.push_back({cur_start_, std::max(t, cur_start_), cur_state_});
}

namespace {
double ms(int64_t ns) { return std::round(double(ns) / 1e3) / 1e3; }
double sec(int64_t ns) { return std::round(double(ns) / 1e6) / 1e3; }
}  // namespace

std::string MetricsSink::report_json() const {
  json::Object o;
  const int64_t t0 = t_min_ == INT64_MAX ? 0 : t_min_;
  json::Object channels;
  for (auto& [name, c] : ch_) {
    json::Object co;
    co.set("n", c.n).set("rejected_non_monotonic", c.rejected);
    if (name != "event" && c.n > 1) {
      double dur = double(c.last - c.first) / 1e9;
      co.set("rate_hz", std::round(double(c.n - 1) / dur * 1000) / 1000);
      co.set("max_gap_ms", ms(c.max_gap));
    }
    channels.set(name, co);
  }
  o.set("channels", channels);
  json::Object ev;
  for (auto& [k, n] : events_) ev.set(k, n);
  o.set("events", ev);
  std::vector<std::string> iv;
  for (auto& i : intervals_) {
    json::Object io;
    io.set("t0_s", sec(i.t0_ns - t0)).set("t1_s", sec(i.t1_ns - t0)).set("state", i.state);
    iv.push_back(io.dump());
  }
  o.raw("tracking_not_normal", json::array(iv.begin(), iv.end()));
  o.set("duration_s", t_max_ >= t0 && t_min_ != INT64_MAX ? sec(t_max_ - t0) : 0.0);
  o.set("path_length_m", std::round(path_m_ * 1e4) / 1e4);
  o.set("keyframes", keyframes_);
  o.set("points_total", points_);
  return o.dump();
}

}  // namespace capturekit::replay
