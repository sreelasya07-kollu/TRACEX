#pragma once
// ReplaySink: anything in the core that consumes a capture stream. The replay driver (ckit replay)
// and the live session (Phase 6) feed sinks the same, t_ns-ordered calls. Sinks never read a clock.
//
// Delivery order for equal t_ns (documented tie-break, docs/replay.md):
//   intrinsics < pose < gyro < accel < frame < event < points < planes < detections
#include <cstdint>
#include <string>

#include "capturekit/bundle/types.hpp"

namespace capturekit::replay {

/// A frame as seen by sinks. luma may be null when no sink asked for images.
struct FrameView {
  int64_t t_ns = 0;
  uint32_t frame_id = 0;
  std::string file;
  bool keyframe = false;  // kept as JPEG in the source bundle
  int64_t exposure_ns = 0;
  const uint8_t* luma = nullptr;
  int width = 0, height = 0, stride = 0;
};

class ReplaySink {
 public:
  virtual ~ReplaySink() = default;
  virtual const char* name() const = 0;
  virtual bool needs_images() const { return false; }

  virtual void on_intrinsics(const bundle::IntrinsicsSample&) {}
  virtual void on_pose(const bundle::PoseSample&) {}
  virtual void on_gyro(const bundle::ImuSample&) {}
  virtual void on_accel(const bundle::ImuSample&) {}
  virtual void on_frame(const FrameView&) {}
  virtual void on_event(const bundle::EventRecord&) {}
  virtual void on_points(const bundle::PointCloudRecord&) {}
  virtual void on_plane(const bundle::PlaneRecord&) {}
  virtual void on_detections(const bundle::DetectionsRecord&) {}
  virtual void on_end(int64_t /*t_ns*/) {}

  /// Deterministic JSON object (sorted keys, no wall-clock values).
  virtual std::string report_json() const = 0;
};

}  // namespace capturekit::replay

#include <memory>
#include <vector>

namespace capturekit::replay {
/// Creates a registered sink ("metrics", later "quality", "governor", "session", "guide", ...).
/// config_json is a flat JSON object of overrides ("{}" for defaults). Throws on unknown name.
std::unique_ptr<ReplaySink> make_sink(const std::string& name, const std::string& config_json);
std::vector<std::string> sink_names();
}  // namespace capturekit::replay
