#include "capturekit/quality/quality_sink.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "capturekit/util/json.hpp"

namespace capturekit::quality {

QualitySink::QualitySink(const QualityConfig& cfg, bool emit)
    : cfg_(cfg), emit_frames_(emit), analyzer_(cfg), selector_(cfg) {
  rows_.reserve(4096);
  decisions_.reserve(64);
}

std::unique_ptr<replay::ReplaySink> QualitySink::create(const std::string& cfg_json) {
  std::string err;
  auto flat = json::parse_flat(cfg_json, &err);
  if (!flat) throw std::invalid_argument("quality config: " + err);
  bool emit = false;
  json::Object rest;
  for (auto& [k, v] : *flat) {
    if (k == "emit_frames") {
      if (auto* d = std::get_if<double>(&v)) emit = *d != 0;
      if (auto* b = std::get_if<bool>(&v)) emit = *b;
    } else if (auto* d = std::get_if<double>(&v)) {
      rest.set(k, *d);
    } else {
      throw std::invalid_argument("quality config: non-numeric value for " + k);
    }
  }
  auto cfg = QualityConfig::from_json(rest.dump(), &err);
  if (!cfg) throw std::invalid_argument(err);
  return std::make_unique<QualitySink>(*cfg, emit);
}

void QualitySink::on_intrinsics(const bundle::IntrinsicsSample& k) { fx_ = k.fx; }

void QualitySink::on_pose(const bundle::PoseSample& p) {
  auto T = geometry::PoseWC::from_tq(p.t_world_from_camera, p.q_world_from_camera_wxyz);
  if (have_pose_ && int(p.tracking_state) == 1) path_m_ += (T.translation() - pose_.translation()).norm();
  pose_ = T;
  state_ = int(p.tracking_state);
  have_pose_ = true;
}

void QualitySink::on_gyro(const bundle::ImuSample& s) { gyro_.push(s.t_ns, s.v[0], s.v[1], s.v[2]); }

void QualitySink::on_points(const bundle::PointCloudRecord& p) {
  if (!have_pose_ || p.xyz.size() < 30) return;
  if (auto d = median_point_depth(pose_, p.xyz.data(), p.xyz.size() / 3))
    selector_.set_scene_depth(std::clamp(*d, 0.5, 6.0));
}

void QualitySink::apply(const std::vector<KeyframeSelector::Decision>& ds) {
  for (auto& d : ds) {
    auto it = row_of_.find(d.frame_id);
    if (it == row_of_.end()) continue;
    if (d.keyframe && !rows_[it->second].keyframe && keyframe_cb_) keyframe_cb_(d.frame_id, d.t_ns, rows_[it->second].m.sharpness_norm);
    rows_[it->second].keyframe = d.keyframe;
    rows_[it->second].decision_reasons = d.reasons;
  }
}

void QualitySink::on_frame(const replay::FrameView& f) {
  if (rows_.empty()) t_first_ = f.t_ns;
  t_last_ = f.t_ns;
  FrameMetrics m;
  if (f.luma) analyzer_.measure({f.luma, f.width, f.height, f.stride}, m);
  else m.reasons |= kNoImage;
  m.blur_risk_px = fx_ > 0 ? blur_risk_px(gyro_, f.t_ns, f.exposure_ns, fx_,
                                          int64_t(cfg_.default_exposure_ms * 1e6))
                           : std::nan("");
  analyzer_.finish(m, have_pose_ ? state_ : 0);
  row_of_[f.frame_id] = rows_.size();
  rows_.push_back({f.frame_id, f.t_ns, m});
  if (frame_cb_) frame_cb_(f.frame_id, f.t_ns, m);
  decisions_.clear();
  selector_.push(f.frame_id, f.t_ns, m, pose_, decisions_);
  apply(decisions_);
}

void QualitySink::on_end(int64_t) {
  decisions_.clear();
  selector_.flush(decisions_);
  apply(decisions_);
}

namespace {
double pct(std::vector<double> v, double p) {
  v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return std::isnan(x); }), v.end());
  if (v.empty()) return std::nan("");
  std::sort(v.begin(), v.end());
  return v[size_t(std::lround(p * double(v.size() - 1)))];
}
double r4(double x) { return std::isnan(x) ? x : std::round(x * 1e4) / 1e4; }
}  // namespace

std::string QualitySink::report_json() const {
  json::Object o;
  o.set("config_version", cfg_.version).set("config_hash", cfg_.hash()).raw("config", cfg_.to_json());
  uint64_t kf = 0, hard = 0, noimg = 0;
  std::map<std::string, uint64_t> hard_hist, all_hist;
  std::vector<double> sn, br, q;
  std::vector<std::string> sel, frames;
  for (auto& r : rows_) {
    kf += r.keyframe;
    hard += r.m.hard_reject;
    noimg += (r.m.reasons & kNoImage) != 0;
    constexpr uint32_t kHardMask = kTrackingLost | kMotionBlur | kBlur | kOverexposed | kUnderexposed | kNoImage;
    for (auto& c : reason_codes(r.decision_reasons | r.m.reasons)) ++all_hist[c];
    if (r.m.hard_reject)
      for (auto& c : reason_codes(r.m.reasons & kHardMask)) ++hard_hist[c];
    if (!(r.m.reasons & kNoImage)) sn.push_back(r.m.sharpness_norm);
    br.push_back(r.m.blur_risk_px);
    if (!r.m.hard_reject) q.push_back(r.m.quality);
    if (r.keyframe) sel.push_back(json::num(int64_t(r.frame_id)));
    if (emit_frames_) {
      json::Object fo;
      std::vector<std::string> rs;
      for (auto& c : reason_codes(r.decision_reasons | r.m.reasons)) rs.push_back(json::str(c));
      fo.set("frame_id", int64_t(r.frame_id)).set("t_ns", r.t_ns).set("keyframe", r.keyframe)
          .set("sharpness", r4(r.m.sharpness)).set("sharpness_norm", r4(r.m.sharpness_norm))
          .set("blur_risk_px", r4(r.m.blur_risk_px)).set("clip_low", r4(r.m.clip_low))
          .set("clip_high", r4(r.m.clip_high)).set("corners", int64_t(r.m.corners))
          .set("corner_spread", r4(r.m.corner_spread)).set("quality", r4(r.m.quality))
          .set("tracking_state", r.m.tracking_state).raw("reasons", json::array(rs.begin(), rs.end()));
      frames.push_back(fo.dump());
    }
  }
  const double dur = double(t_last_ - t_first_) / 1e9;
  o.set("frames", uint64_t(rows_.size())).set("keyframes", kf).set("hard_rejected", hard)
      .set("no_image", noimg);
  o.set("keyframes_per_s", dur > 0 ? r4(double(kf) / dur) : 0.0);
  o.set("keyframes_per_m", path_m_ > 0 ? r4(double(kf) / path_m_) : 0.0);
  o.set("path_m", r4(path_m_));
  json::Object hh, ah;
  for (auto& [k, v] : hard_hist) hh.set(k, v);
  for (auto& [k, v] : all_hist) ah.set(k, v);
  o.set("hard_reject_histogram", hh).set("reason_histogram", ah);
  json::Object st;
  st.set("sharpness_norm_p10", r4(pct(sn, 0.1))).set("sharpness_norm_p50", r4(pct(sn, 0.5)))
      .set("blur_risk_px_p50", r4(pct(br, 0.5))).set("blur_risk_px_p90", r4(pct(br, 0.9)))
      .set("quality_p50", r4(pct(q, 0.5)));
  o.set("stats", st);
  o.raw("selected_frame_ids", json::array(sel.begin(), sel.end()));
  if (emit_frames_) o.raw("per_frame", json::array(frames.begin(), frames.end()));
  return o.dump();
}

}  // namespace capturekit::quality
