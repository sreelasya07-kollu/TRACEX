#pragma once
// Capture session (Phase 6): the engine behind the C API (capi/capturekit.h).
// State machine: Idle -> Calibrating -> Scanning <-> Paused -> Finalizing -> Complete | Failed.
// Ingest calls only copy into bounded queues; one consumer (worker thread or pump()) owns the
// stream writer, quality analyzer, keyframe selector and governor.
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "capturekit/bundle/finalize.hpp"
#include "capturekit/bundle/stream_writer.hpp"
#include "capturekit/bundle/validate.hpp"
#include "capturekit/governor/governor.hpp"
#include "capturekit/guide/engine.hpp"
#include "capturekit/quality/quality.hpp"

namespace capturekit::session {

enum class State : int { Idle = 0, Calibrating, Scanning, Paused, Finalizing, Complete, Failed };
enum class Status : int { Ok = 0, ErrState = 1, ErrArg = 2, ErrIo = 3, ErrPolicy = 4, ErrFull = 5 };
const char* state_name(State s);

/// Legal user-driven transitions (worker-driven ones: Calibrating->Scanning, governor pause).
bool transition_allowed(State from, State to);

struct SessionConfig {
  bool threaded = true;
  int frame_slots = 4;
  size_t queue_capacity = 8192;
  int calib_normal_poses = 15;
  double keyframe_ack_timeout_s = 2.0;
  size_t outbound_capacity = 1024;
  /// ADR 0034 (lean bundle): /imu/gyro and /imu/accel are written only when true (research). Off by default: nothing
  /// downstream reads them. The core still keeps the gyro in memory for blur risk and the tracking-recovery check.
  bool record_imu = false;
  bundle::BundleMeta meta;
  quality::QualityConfig quality;
  governor::GovernorConfig governor;
  guide::GuideParams guide;
  static std::optional<SessionConfig> from_json(std::string_view flat_json, std::string* err);
};

struct FrameInput {
  int64_t t_ns = 0;
  uint32_t frame_id = 0;
  int64_t exposure_ns = 0;
  const uint8_t* luma = nullptr;
  int width = 0, height = 0, stride = 0;
};

struct Counters {
  uint64_t ingested[9] = {}, dropped[9] = {};  // index by Kind
};

class Session {
 public:
  enum Kind : int { kPose = 0, kGyro, kAccel, kIntrinsics, kFrame, kPoints, kPlane, kPlatform, kDetections };

  static std::unique_ptr<Session> create(const std::string& dir, const SessionConfig& cfg, std::string* err);
  ~Session();  // stops the worker; without finalize() the bundle stays recoverable

  Status start(int64_t t_ns);
  Status pause(int64_t t_ns);
  Status resume(int64_t t_ns);
  Status stop(int64_t t_ns);
  Status finalize();
  State state() const { return state_.load(); }
  Status pump();

  Status ingest_pose(const bundle::PoseSample& p);
  Status ingest_gyro(const bundle::ImuSample& s);
  Status ingest_accel(const bundle::ImuSample& s);
  Status ingest_intrinsics(const bundle::IntrinsicsSample& k);
  Status ingest_frame(const FrameInput& f);
  Status ingest_points(const bundle::PointCloudRecord& p);
  Status ingest_plane(const bundle::PlaneRecord& p);
  /// One keyframe's boxes (may be empty); ErrArg above kMaxDetectionBoxes. Bounded queue, drops counted.
  Status ingest_detections(const bundle::DetectionsRecord& d);
  static constexpr size_t kMaxDetectionBoxes = 256;
  Status ingest_platform(int64_t t_ns, const governor::Signals& s);
  Status keyframe_written(uint32_t frame_id, bool ok);

  /// Pops whole JSON lines up to max_bytes (at least one if any is pending and fits).
  std::string poll_events(size_t max_bytes);
  governor::Policy policy() const;
  Counters counters() const;
  const std::string& dir() const { return dir_; }

 private:
  Session(const std::string& dir, const SessionConfig& cfg);
  struct Control {
    enum Type { Start, Pause, Resume, Stop, GovernorPause } type;
    int64_t t_ns;
  };
  struct FrameItem {
    FrameInput meta;
    int slot;
  };
  struct Ack {
    uint32_t frame_id;
    bool ok;
  };
  struct Platform {
    int64_t t_ns;
    governor::Signals s;
  };
  struct Gyro {
    bundle::ImuSample s;
  };
  struct Accel {
    bundle::ImuSample s;
  };
  using Item = std::variant<bundle::PoseSample, Gyro, Accel, bundle::IntrinsicsSample, FrameItem,
                            bundle::PointCloudRecord, bundle::PlaneRecord, bundle::DetectionsRecord, Platform, Control, Ack>;
  struct Pending {
    bundle::FrameRecord rec;
    bool decided = false, awaiting_ack = false;
  };

  Status enqueue(Item&& it, int kind, bool force = false);
  /// t_ns <= 0 is a platform clock bug (mapped timestamp missing): counted as an ingest drop, never recorded.
  Status reject_time(int kind);
  void worker_loop();
  void process(Item& it);
  void on_frame(FrameItem& f);
  void forward_detections(const bundle::DetectionsRecord& d);  // -> guide_.on_detections
  void on_decisions(const std::vector<quality::KeyframeSelector::Decision>& ds);
  void flush_ready(bool force_all);
  void expire_acks(int64_t now_t);
  void governor_step(int64_t t);
  void guide_tick(int64_t t);
  void publish_recovery();  // tracking_lost / tracking_reset events + /events records (ADR 0031)
  void write_guide_outputs(int64_t t, bool final_pass);  // /guide/labels, /guide/shell, /guide/gaps (ADR 0028)
  void write_event(int64_t t, const std::string& source, const std::string& code, const std::string& msg,
                   std::vector<std::pair<std::string, std::string>> attrs = {});
  void emit(const std::string& json_line, bool critical);
  void set_state(State to, int64_t t, const std::string& reason);
  void report_drops(int64_t t);
  bool write_checkpoint();

  std::string dir_;
  SessionConfig cfg_;
  std::atomic<State> state_{State::Idle};

  // Ingest side
  mutable std::mutex qmu_;
  std::condition_variable qcv_;
  std::deque<Item> queue_;
  bool stop_worker_ = false;
  std::vector<std::vector<uint8_t>> slots_;
  std::vector<int> free_slots_;
  std::atomic<uint64_t> ingested_[9] = {}, dropped_[9] = {};
  uint64_t reported_drops_[9] = {};
  std::thread worker_;

  // Outbound events
  mutable std::mutex omu_;
  std::deque<std::string> outbound_;
  uint64_t outbound_dropped_ = 0;

  // Consumer side (worker only)
  bundle::StreamWriter writer_;
  quality::FrameAnalyzer analyzer_;
  quality::KeyframeSelector selector_;
  quality::ImuRing gyro_;
  governor::Governor governor_;
  guide::GuideEngine guide_;
  std::vector<guide::Instruction> instr_scratch_;
  governor::Signals signals_;
  mutable std::mutex pmu_;
  governor::Policy policy_;
  std::optional<bundle::IntrinsicsSample> first_intrinsics_;
  double fx_ = 0;
  geometry::PoseWC pose_;
  int tracking_ = 0, normal_run_ = 0;
  int64_t last_t_ = 0, last_event_t_ = INT64_MIN, last_frame_t_ = 0;
  int64_t last_overlay_t_ = -1;  // event time of the last "overlay" event (500 ms cadence)
  int64_t last_outputs_t_ = -1, outputs_written_t_ = 0;  // guide output channels: last check / last written event time
  std::string out_key_[3];                               // encoded (t_ns = 0) last written labels / shell / gaps
  std::map<int64_t, Pending> reorder_;
  std::map<uint32_t, int64_t> t_of_frame_;
  uint32_t next_keyframe_ = 0;
  std::array<bool, 30> recent_drop_{};
  size_t recent_idx_ = 0;
  std::vector<quality::KeyframeSelector::Decision> decisions_;
  std::vector<governor::Transition> transitions_;
};

/// Recovery report from recover().
struct RecoverResult {
  bool ok = false;
  bool already_complete = false;
  uint64_t poses = 0, frames = 0, keyframes = 0, dropped_frame_records = 0, removed_orphans = 0;
  std::string error;
  std::string to_json() const;
};
/// Turns an interrupted bundle into a valid partial bundle (completion "recovered").
RecoverResult recover(const std::string& dir);

}  // namespace capturekit::session
