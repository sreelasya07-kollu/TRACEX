#include "capturekit/guide/guide_sink.hpp"

#include <cmath>
#include <stdexcept>

#include "capturekit/util/json.hpp"

namespace capturekit::guide {

GuideSink::GuideSink(const GuideParams& gp, const quality::QualityConfig& qc)
    : quality_(qc, false), engine_(gp) {
  quality_.set_observers(
      [this](uint32_t id, int64_t t, const quality::FrameMetrics& m) {
        FrameObs o;
        o.t_ns = t, o.frame_id = id, o.hard_reject = m.hard_reject;
        o.motion_blur = (m.reasons & quality::kMotionBlur) != 0;
        o.underexposed = (m.reasons & quality::kUnderexposed) != 0;
        o.sharpness_norm = m.sharpness_norm;
        engine_.on_frame(o);
      },
      [this](uint32_t id, int64_t t, double sharp) { engine_.on_keyframe(id, t, sharp); });
}

std::unique_ptr<replay::ReplaySink> GuideSink::create(const std::string& cfg_json) {
  std::string err;
  auto flat = json::parse_flat(cfg_json.empty() ? "{}" : cfg_json, &err);
  if (!flat) throw std::invalid_argument("guide config: " + err);
  json::Object g, q;
  for (auto& [k, v] : *flat) {
    auto* d = std::get_if<double>(&v);
    if (!d) throw std::invalid_argument("guide config: non-numeric value for " + k);
    if (k.rfind("quality.", 0) == 0) q.set(k.substr(8), *d);
    else g.set(k, *d);
  }
  auto gp = GuideParams::from_json(g.dump(), &err);
  if (!gp) throw std::invalid_argument(err);
  auto qc = quality::QualityConfig::from_json(q.dump(), &err);
  if (!qc) throw std::invalid_argument(err);
  return std::make_unique<GuideSink>(*gp, *qc);
}

void GuideSink::on_intrinsics(const bundle::IntrinsicsSample& k) {
  quality_.on_intrinsics(k);
  engine_.on_intrinsics(k);
}

void GuideSink::on_points(const bundle::PointCloudRecord& p) {
  quality_.on_points(p);
  engine_.on_points(p);
}

void GuideSink::on_pose(const bundle::PoseSample& p) {
  if (t_first_ < 0) t_first_ = p.t_ns;
  quality_.on_pose(p);
  engine_.on_pose(p);
  engine_.update(p.t_ns, issued_);
}

void GuideSink::on_end(int64_t t) { quality_.on_end(t); }

std::string GuideSink::report_json() const {
  json::Object o;
  o.raw("engine", engine_.report_json());
  std::vector<std::string> ins;
  for (size_t i = 0; i < issued_.size() && i < 100; ++i) {
    auto& x = issued_[i];
    json::Object io;
    io.set("t_s", std::round(double(x.t_ns - t_first_) / 1e6) / 1e3).set("type", type_name(x.type)).set("text", x.speak_text);
    ins.push_back(io.dump());
  }
  o.raw("first_instructions", json::array(ins.begin(), ins.end()));
  return o.dump();
}

}  // namespace capturekit::guide
