#pragma once
// Quality metrics and keyframe selection (Phase 4). Pure C++ on a luma plane + metadata.
// Hot path (FrameAnalyzer::measure/finish, ImuRing, blur_risk_px) does no heap allocation once
// the buffers are sized for a resolution.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "capturekit/geometry/geometry.hpp"

namespace capturekit::quality {

// ---- Reason codes (bit mask). Strings are the codes written to /frame_index.reasons ----
enum Reason : uint32_t {
  kTrackingLost = 1u << 0,      // hard
  kTrackingLimited = 1u << 1,   // soft
  kMotionBlur = 1u << 2,        // hard above blur_reject_px, soft above blur_soft_px
  kBlur = 1u << 3,              // hard: textured scene but sharpness_norm < sharp_reject
  kSoftFocus = 1u << 4,         // soft: sharpness_norm < sharp_soft
  kOverexposed = 1u << 5,       // hard >= clip_reject, soft >= clip_soft
  kUnderexposed = 1u << 6,      // hard >= clip_reject, soft >= clip_soft
  kLowTexture = 1u << 7,        // soft: blank wall / few corners (never treated as blur)
  kPoorSpread = 1u << 8,        // soft: corners in few grid cells
  kNoImage = 1u << 9,           // frame had no pixels to measure
  kNotNovel = 1u << 10,         // selector: not enough motion since last keyframe
  kMinGap = 1u << 11,           // selector: too soon after last keyframe
  kNotBest = 1u << 12,          // selector: a better frame won the candidate window
  kRecalibrating = 1u << 13,    // hard (session): tracking recovery active, keyframe capture paused (ADR 0031)
  kDuplicate = 1u << 14,        // selector: an older saved keyframe already covers this view (revisit, ADR 0033)
};
constexpr uint32_t kLastReason = kDuplicate;  // highest bit, for iteration
const char* reason_code(Reason r);
std::vector<std::string> reason_codes(uint32_t mask);  // reporting only (allocates)

/// Versioned thresholds. Starting guesses (AGENTS.md): tune on recorded data.
struct QualityConfig {
  std::string version = "q1";
  double target_width = 320;     // downscale target (px)
  double fast_threshold = 20;    // FAST-9 intensity threshold
  double corner_cell_min = 2;    // corners for a 4x4 grid cell to count as occupied
  double min_corners = 40;
  double min_spread = 0.4;
  double texture_min = 25;       // coarse-scale Laplacian variance below this = low texture
  double sharp_reject = 0.35;    // sharpness_norm hard reject (textured scenes only)
  double sharp_soft = 0.6;
  double baseline_up = 0.2;      // running baseline tracks the upper envelope of sharpness ratio
  double baseline_down = 0.02;
  double clip_soft = 0.10;
  double clip_reject = 0.40;
  double blur_soft_px = 1.5;
  double blur_reject_px = 4.0;
  double default_exposure_ms = 16;
  double limited_penalty = 0.6;
  // Keyframe selector
  double scene_depth_m = 2.0;    // indoor default when no depth estimate is available
  double trans_ratio = 0.10;     // novelty: translation / scene depth
  double rot_deg = 10.0;         // novelty: rotation since last keyframe
  double min_gap_ms = 200;
  double window_ms = 300;

  static std::optional<QualityConfig> from_json(std::string_view flat_json, std::string* err = nullptr);
  std::string to_json() const;  // canonical, sorted keys
  std::string hash() const;     // first 16 hex chars of sha256(to_json())
};

struct LumaView {
  const uint8_t* data = nullptr;
  int width = 0, height = 0, stride = 0;
};

struct FrameMetrics {
  double sharpness = 0;       // variance of Laplacian on the ~320 px image
  double texture = 0;         // variance of Laplacian on a further 4x downscale (blur-insensitive)
  double sharpness_norm = 0;  // (sharpness / texture) / running session baseline
  double blur_risk_px = 0;    // gyro-predicted blur in px (NaN = unknown)
  double clip_low = 0, clip_high = 0, mean_luma = 0;
  uint32_t corners = 0;
  double corner_spread = 0;
  int tracking_state = 0;
  double quality = 0;  // 0..1, 0 when hard rejected
  uint32_t reasons = 0;
  bool hard_reject = false;
};

/// Fixed-capacity ring of gyro magnitudes (rad/s).
class ImuRing {
 public:
  void push(int64_t t_ns, double wx, double wy, double wz);
  size_t size() const { return n_; }
  /// Mean |w| for samples in [t0, t1]; falls back to the nearest sample within 10 ms; NaN if none.
  double mean_rate(int64_t t0, int64_t t1) const;

 private:
  static constexpr size_t kCap = 1024;
  std::array<int64_t, kCap> t_{};
  std::array<double, kCap> w_{};
  size_t head_ = 0, n_ = 0;
};

/// Gyro motion-blur risk in pixels over the exposure window centred on t_ns.
double blur_risk_px(const ImuRing& gyro, int64_t t_ns, int64_t exposure_ns, double focal_px,
                    int64_t default_exposure_ns);

/// Median depth (m) of sparse world points in front of the camera; nullopt if < 10 such points.
std::optional<double> median_point_depth(const geometry::PoseWC& T_world_from_camera, const float* xyz,
                                         size_t n_points);

class FrameAnalyzer {
 public:
  explicit FrameAnalyzer(const QualityConfig& cfg) : cfg_(cfg) {}
  /// Image metrics: sharpness, texture, clipping, mean, corners, spread.
  void measure(const LumaView& img, FrameMetrics& m);
  /// Normalise sharpness against the session baseline and score (quality, reasons, hard_reject).
  void finish(FrameMetrics& m, int tracking_state);
  double baseline() const { return baseline_; }
  const QualityConfig& config() const { return cfg_; }

 private:
  QualityConfig cfg_;
  std::vector<uint8_t> small_, coarse_;
  std::vector<uint32_t> acc_;
  int sw_ = 0, sh_ = 0, cw_ = 0, ch_ = 0;
  double baseline_ = 0;
};

class KeyframeSelector {
 public:
  struct Decision {
    uint32_t frame_id = 0;
    int64_t t_ns = 0;
    bool keyframe = false;
    double quality = 0;
    uint32_t reasons = 0;  // metric reasons | selector reasons
  };
  /// Saved keyframe poses kept for revisit detection (ADR 0033): the newest kPoseIndexCap, oldest evicted.
  static constexpr size_t kPoseIndexCap = 2048;
  explicit KeyframeSelector(const QualityConfig& cfg) : cfg_(cfg) {
    pending_.reserve(64);
    index_.reserve(kPoseIndexCap);
  }
  /// Every pushed frame gets exactly one decision, possibly after its candidate window closes.
  void push(uint32_t frame_id, int64_t t_ns, const FrameMetrics& m,
            const geometry::PoseWC& T_world_from_camera, std::vector<Decision>& out);
  void flush(std::vector<Decision>& out);
  void set_scene_depth(double metres) { depth_ = metres; }
  /// Governor keyframe_rate_scale: min gap is divided by it; <= 0 stops new keyframes.
  void set_rate_scale(double s) { rate_scale_ = s; }
  uint64_t keyframes() const { return n_kf_; }
  size_t indexed_keyframes() const { return index_.size(); }
  /// Forget all saved keyframe poses (and the last keyframe for novelty); capacity is kept, no allocation.
  void clear_index();
  /// Tracking re-anchor (ADR 0031 TRACKING_RESET): old poses live in a shifted frame. Clears the index when the
  /// drift exceeds the dedup thresholds (drift_m / depth >= trans_ratio or drift_deg >= rot_deg). True if cleared.
  bool reanchor(double drift_m, double drift_deg);

 private:
  struct Cand {
    Decision d;
    geometry::PoseWC pose;
  };
  struct KfPose {
    Eigen::Vector3d t;
    Eigen::Quaterniond q;
  };
  void close_window(std::vector<Decision>& out);
  /// 0 when T is novel; kNotNovel when the last keyframe covers it, kDuplicate when an older saved one does.
  uint32_t novelty(const geometry::PoseWC& T) const;
  QualityConfig cfg_;
  double depth_ = 0, rate_scale_ = 1.0;
  bool have_kf_ = false, window_ = false;
  geometry::PoseWC kf_pose_;
  int64_t kf_t_ = 0, window_t0_ = 0;
  Cand best_;
  std::vector<Decision> pending_;
  std::vector<KfPose> index_;  // ring once full; index_head_ = next slot to overwrite
  size_t index_head_ = 0;
  uint64_t n_kf_ = 0;
};

}  // namespace capturekit::quality
