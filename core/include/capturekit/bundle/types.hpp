#pragma once
// Plain records carried by the Scan Bundle channels (spec/scanbundle/SPEC.md).
// Poses are T_world_from_camera in our conventions (docs/program-spec/conventions.md).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace capturekit::bundle {

enum class TrackingState : int { Unspecified = 0, Normal = 1, Limited = 2, NotAvailable = 3 };
enum class TrackingReason : int {
  None = 0, Initializing = 1, ExcessiveMotion = 2, InsufficientFeatures = 3,
  InsufficientLight = 4, Relocalizing = 5, BadState = 6, CameraUnavailable = 7, Other = 8
};

/// One /pose sample: T_world_from_camera as translation + unit quaternion (w,x,y,z).
struct PoseSample {
  int64_t t_ns = 0;
  double t_world_from_camera[3] = {0, 0, 0};
  double q_world_from_camera_wxyz[4] = {1, 0, 0, 0};
  TrackingState tracking_state = TrackingState::Normal;
  TrackingReason tracking_reason = TrackingReason::None;
};

struct ImuSample {
  int64_t t_ns = 0;
  double v[3] = {0, 0, 0};
};

enum class CameraModelId : int { Pinhole = 0, OpenCV = 1 };

struct IntrinsicsSample {
  int64_t t_ns = 0;
  uint32_t width = 0, height = 0;
  double fx = 0, fy = 0, cx = 0, cy = 0;
  CameraModelId model = CameraModelId::Pinhole;
  std::vector<double> distortion;
};

struct FrameRecord {
  int64_t t_ns = 0;
  uint32_t frame_id = 0;
  std::string file;  // bundle-relative, empty when not kept
  bool keyframe = false;
  int64_t exposure_ns = 0;
  double sharpness = 0, sharpness_norm = 0, blur_risk_px = 0, clip_low = 0, clip_high = 0;
  uint32_t corners = 0;
  double corner_spread = 0, quality = 0;
  std::vector<std::string> reasons;
  TrackingState tracking_state = TrackingState::Unspecified;
};

struct EventRecord {
  int64_t t_ns = 0;
  std::string source, code, message;
  std::vector<std::pair<std::string, std::string>> attrs;
};

struct PointCloudRecord {
  int64_t t_ns = 0;
  std::vector<float> xyz;
};

struct PlaneRecord {
  int64_t t_ns = 0;
  std::string id, kind;
  double center[3] = {0, 0, 0}, normal[3] = {0, 1, 0}, axis_u[3] = {1, 0, 0};
  double extent_u = 0, extent_v = 0;
};

/// One 2D box on a keyframe image (pixels of the stored JPEG). cls = core ObjectClass id.
struct DetectionBox {
  int32_t cls = 0;
  float score = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

/// /ar/detections: all boxes of one keyframe (schema 1.1.0, ADR 0016).
struct DetectionsRecord {
  int64_t t_ns = 0;
  uint32_t frame_id = 0;
  std::vector<DetectionBox> boxes;
};

/// /guide/labels (schema 1.3.0, ADR 0028): objects and openings the guidance engine had published, world metres.
/// Derived output (replay recomputes it, never feeds it back). Plain copies of guide::Object / guide::Opening.
struct LabelObject {
  uint32_t id = 0;
  int32_t cls = 0;  // core ObjectClass id
  double centre[3] = {0, 0, 0}, size[3] = {0, 0, 0};  // size = width, height, depth (box frame)
  double yaw = 0, conf = 0;
  uint32_t n_obs = 0;
};
struct LabelOpening {
  int32_t kind = 0;  // 0 door, 1 window
  std::string wall;  // shell wall id, may be empty
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0, y0 = 0, y1 = 0, conf = 0;
};
struct LabelsRecord {
  int64_t t_ns = 0;
  std::vector<LabelObject> objects;
  std::vector<LabelOpening> openings;
};

/// /guide/shell (schema 1.3.0): the live room shell.
struct ShellWallItem {
  std::string id;
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  bool inferred = false;
};
struct ShellRecord {
  int64_t t_ns = 0;
  std::vector<ShellWallItem> walls;
  std::vector<double> corners;  // x0,z0,x1,z1,...
  bool closed = false, low_confidence = false;
  double ceiling_h = 0;
  bool ceiling_inferred = false;
};

/// /guide/gaps (schema 1.3.0): expected-but-unobserved areas.
struct GapItem {
  int32_t type = 0;  // guide::GapType
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0, pri = 0;
  std::string why;
};
struct GapsRecord {
  int64_t t_ns = 0;
  std::vector<GapItem> gaps;
};

namespace topic {
inline constexpr const char* kPose = "/pose";
inline constexpr const char* kGyro = "/imu/gyro";
inline constexpr const char* kAccel = "/imu/accel";
inline constexpr const char* kIntrinsics = "/intrinsics";
inline constexpr const char* kFrameIndex = "/frame_index";
inline constexpr const char* kEvents = "/events";
inline constexpr const char* kPoints = "/ar/points";
inline constexpr const char* kPlanes = "/ar/planes";
inline constexpr const char* kDetections = "/ar/detections";
inline constexpr const char* kGuideLabels = "/guide/labels";
inline constexpr const char* kGuideShell = "/guide/shell";
inline constexpr const char* kGuideGaps = "/guide/gaps";
}  // namespace topic

}  // namespace capturekit::bundle
