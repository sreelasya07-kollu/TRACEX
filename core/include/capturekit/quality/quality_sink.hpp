#pragma once
// QualitySink: per-frame quality metrics + keyframe selection as a ReplaySink ("quality").
// Config: QualityConfig keys plus "emit_frames" (1 = include per-frame records in the report).
#include <functional>
#include <map>
#include <vector>

#include "capturekit/quality/quality.hpp"
#include "capturekit/replay/sink.hpp"

namespace capturekit::quality {

class QualitySink : public replay::ReplaySink {
 public:
  explicit QualitySink(const QualityConfig& cfg, bool emit_frames = false);
  static std::unique_ptr<replay::ReplaySink> create(const std::string& config_json);

  /// Observers used by the guide sink: per-frame metrics (as soon as measured) and keyframe decisions.
  using FrameCb = std::function<void(uint32_t frame_id, int64_t t_ns, const FrameMetrics&)>;
  using KeyframeCb = std::function<void(uint32_t frame_id, int64_t t_ns, double sharpness_norm)>;
  void set_observers(FrameCb f, KeyframeCb k) { frame_cb_ = std::move(f), keyframe_cb_ = std::move(k); }

  const char* name() const override { return "quality"; }
  bool needs_images() const override { return true; }
  void on_intrinsics(const bundle::IntrinsicsSample& k) override;
  void on_pose(const bundle::PoseSample& p) override;
  void on_gyro(const bundle::ImuSample& s) override;
  void on_frame(const replay::FrameView& f) override;
  void on_points(const bundle::PointCloudRecord& p) override;
  void on_end(int64_t t_ns) override;
  std::string report_json() const override;

 private:
  struct Row {
    uint32_t frame_id;
    int64_t t_ns;
    FrameMetrics m;
    bool keyframe = false;
    uint32_t decision_reasons = 0;
  };
  QualityConfig cfg_;
  bool emit_frames_;
  FrameAnalyzer analyzer_;
  KeyframeSelector selector_;
  ImuRing gyro_;
  double fx_ = 0;
  geometry::PoseWC pose_;
  int state_ = 0;
  bool have_pose_ = false;
  double path_m_ = 0;
  int64_t t_first_ = 0, t_last_ = 0;
  std::vector<Row> rows_;
  std::map<uint32_t, size_t> row_of_;
  std::vector<KeyframeSelector::Decision> decisions_;
  FrameCb frame_cb_;
  KeyframeCb keyframe_cb_;
  void apply(const std::vector<KeyframeSelector::Decision>& d);
};

}  // namespace capturekit::quality
