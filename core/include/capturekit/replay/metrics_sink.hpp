#pragma once
// MetricsSink: stream health metrics for replay reports (counts, rates, gaps, tracking intervals,
// rejected non-monotonic samples, event codes, path length).
#include <map>
#include <vector>

#include "capturekit/replay/sink.hpp"

namespace capturekit::replay {

class MetricsSink : public ReplaySink {
 public:
  const char* name() const override { return "metrics"; }
  void on_intrinsics(const bundle::IntrinsicsSample& k) override;
  void on_pose(const bundle::PoseSample& p) override;
  void on_gyro(const bundle::ImuSample& s) override;
  void on_accel(const bundle::ImuSample& s) override;
  void on_frame(const FrameView& f) override;
  void on_event(const bundle::EventRecord& e) override;
  void on_points(const bundle::PointCloudRecord& p) override;
  void on_plane(const bundle::PlaneRecord& p) override;
  void on_detections(const bundle::DetectionsRecord& d) override;
  void on_end(int64_t t_ns) override;
  std::string report_json() const override;

  struct Interval {
    int64_t t0_ns, t1_ns;
    int state;
  };
  const std::vector<Interval>& tracking_intervals() const { return intervals_; }
  uint64_t rejected(const std::string& channel) const;

 private:
  struct Channel {
    uint64_t n = 0, rejected = 0;
    int64_t first = 0, last = 0, max_gap = 0;
  };
  bool accept(const char* ch, int64_t t_ns);  // per-channel strict monotonicity
  std::map<std::string, Channel> ch_;
  std::map<std::string, uint64_t> events_;
  std::vector<Interval> intervals_;
  int cur_state_ = 1;
  int64_t cur_start_ = 0;
  bool have_pose_ = false;
  double last_p_[3] = {0, 0, 0};
  double path_m_ = 0;
  uint64_t keyframes_ = 0, points_ = 0;
  int64_t t_min_ = INT64_MAX, t_max_ = INT64_MIN, t_end_ = 0;
  bool ended_ = false;
};

}  // namespace capturekit::replay
