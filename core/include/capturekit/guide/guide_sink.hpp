#pragma once
// GuideSink ("guide"): runs the GuideEngine on a replayed stream. Wraps a QualitySink so frame quality
// and keyframe decisions come from the real quality module. Config: flat numeric JSON, keys are
// GuideParams; keys prefixed "quality." go to the quality config.
#include <memory>
#include <vector>

#include "capturekit/guide/engine.hpp"
#include "capturekit/quality/quality_sink.hpp"
#include "capturekit/replay/sink.hpp"

namespace capturekit::guide {

class GuideSink : public replay::ReplaySink {
 public:
  GuideSink(const GuideParams& gp, const quality::QualityConfig& qc);
  static std::unique_ptr<replay::ReplaySink> create(const std::string& config_json);

  const char* name() const override { return "guide"; }
  bool needs_images() const override { return true; }
  void on_intrinsics(const bundle::IntrinsicsSample& k) override;
  void on_pose(const bundle::PoseSample& p) override;
  void on_gyro(const bundle::ImuSample& s) override {
    quality_.on_gyro(s);
    engine_.on_gyro(s);  // tracking-recovery steadiness check (ADR 0031), as in the live session
  }
  void on_frame(const replay::FrameView& f) override { quality_.on_frame(f); }
  void on_points(const bundle::PointCloudRecord& p) override;
  void on_plane(const bundle::PlaneRecord& p) override { engine_.on_plane(p); }
  void on_detections(const bundle::DetectionsRecord& d) override { engine_.on_detections(d); }
  void on_end(int64_t t_ns) override;
  std::string report_json() const override;
  const GuideEngine& engine() const { return engine_; }

 private:
  quality::QualitySink quality_;
  GuideEngine engine_;
  std::vector<Instruction> issued_;
  int64_t t_first_ = -1;
};

}  // namespace capturekit::guide
