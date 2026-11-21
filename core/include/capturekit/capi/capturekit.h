/* CaptureKit C API (Phase 6): the only interface between the native shells (Swift, Kotlin/JNI) and
 * the shared C++ core. Stable, C-only, no C++ types. Frame conventions: docs/program-spec/conventions.md.
 *
 * Threading: ck_ingest_* never block on processing. They copy the input into bounded queues and return
 * CK_ERR_FULL when a queue or frame slot is full (the drop is counted and logged to /events as
 * INGEST_DROP). Processing runs on a core worker thread ("threaded":1, default) or on calls to
 * ck_session_pump() ("threaded":0, used by tests and replay).
 *
 * Time: every call carries t_ns on the canonical clock (mapped at the edge). The core never reads a clock.
 *
 * Keyframes: the core selects keyframes and emits {"type":"keyframe_request","frame_id":..,"file":..}.
 * The shell keeps a short ring of recent camera images, encodes the requested frame as JPEG to
 * <bundle>/<file> (write temp + rename), then calls ck_keyframe_written(). Unconfirmed requests expire
 * after keyframe_ack_timeout_s of frame time and are recorded as ENCODE_MISSING.
 *
 * Lifecycle: create -> start -> (pause/resume)* -> stop -> finalize -> destroy.
 * destroy without finalize leaves a recoverable partial bundle (ck_recover).
 */
#ifndef CAPTUREKIT_CAPI_H
#define CAPTUREKIT_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ck_session ck_session;

typedef enum ck_status {
  CK_OK = 0,
  CK_ERR_STATE = 1,   /* call not legal in the current session state */
  CK_ERR_ARG = 2,     /* invalid argument */
  CK_ERR_IO = 3,      /* filesystem / bundle write failure */
  CK_ERR_POLICY = 4,  /* refused by governor policy (e.g. resume while must_pause) */
  CK_ERR_FULL = 5     /* bounded queue or frame slot full: input dropped and counted */
} ck_status;

typedef enum ck_state {
  CK_STATE_IDLE = 0,
  CK_STATE_CALIBRATING = 1,
  CK_STATE_SCANNING = 2,
  CK_STATE_PAUSED = 3,
  CK_STATE_FINALIZING = 4,
  CK_STATE_COMPLETE = 5,
  CK_STATE_FAILED = 6
} ck_state;

/* Pose conventions accepted at the edge (conversion happens once, in the core). */
typedef enum ck_pose_convention {
  CK_POSE_OPENCV = 0,     /* T_world_from_camera, OpenCV camera axes (internal) */
  CK_POSE_ARKIT_GL = 1    /* ARKit ARCamera.transform / ARCore sensor-oriented Pose: GL camera axes */
} ck_pose_convention;

typedef struct ck_pose {
  int64_t t_ns;
  double t[3];          /* world position (m), Y up */
  double q_wxyz[4];     /* world_from_camera rotation, [w,x,y,z] (ARCore x,y,z,w must be reordered) */
  int32_t tracking_state;   /* 1 NORMAL, 2 LIMITED, 3 NOT_AVAILABLE */
  int32_t tracking_reason;  /* conventions.md tracking_reason codes */
} ck_pose;

typedef struct ck_imu {
  int64_t t_ns;
  double v[3];  /* gyro rad/s or accel m/s^2, IMU frame as delivered */
} ck_imu;

typedef struct ck_intrinsics {
  int64_t t_ns;
  uint32_t width, height;  /* of the stored keyframe image */
  double fx, fy, cx, cy;   /* OpenCV pixel convention */
} ck_intrinsics;

typedef struct ck_frame {
  int64_t t_ns;
  uint32_t frame_id;     /* unique, increasing; the shell keys its image ring by it */
  int64_t exposure_ns;   /* 0 if unknown */
  const uint8_t* luma;   /* Y plane (copied by the call) */
  int32_t width, height, stride;
} ck_frame;

typedef struct ck_platform {
  int32_t thermal_level;   /* 0..3 (ADR 0007) */
  int32_t low_power_mode;
  double battery_pct;
  int32_t charging;
  double frame_time_ms;    /* shell-measured per-frame processing time (EMA) */
} ck_platform;

typedef struct ck_plane {
  int64_t t_ns;
  const char* id;
  const char* kind;  /* floor | wall | ceiling | table | other */
  double center[3], normal[3], axis_u[3];
  double extent_u, extent_v;
} ck_plane;

/* One 2D box on a stored keyframe JPEG (pixels of that image). cls = core ObjectClass id
 * (0 unknown, 1 chair, 2 couch, 3 bed, 4 dining_table, 5 tv, 6 potted_plant, 7 laptop). */
typedef struct ck_detection {
  int32_t cls;
  float score;
  float x0, y0, x1, y1;
} ck_detection;

typedef struct ck_policy {
  int32_t level;  /* 0 NOMINAL .. 5 SAFE_PAUSE */
  double keyframe_rate_scale;
  double ml_rate_scale;
  int32_t capture_resolution_tier;
  int32_t overlay_enabled;
  int32_t upload_allowed;
  int32_t must_pause;
} ck_policy;

const char* ck_version(void);

/* config_json: flat JSON. Keys: threaded, frame_slots, queue_capacity, calib_normal_poses,
 * keyframe_ack_timeout_s, string metadata (bundle_id, session_id, platform, model, os, app_version,
 * clock_base), consent_version + consent_at (ISO-8601 text, both or neither), redaction_mode
 * ("none" | "faces"), record_imu (0 | 1, default 0: /imu/* are not written, ADR 0034; schema 1.4.0),
 * "quality.<key>" and "governor.<key>" overrides. NULL or "{}" for defaults.
 * Creates bundle_dir if needed. Returns NULL on error (status in *err if non-NULL). */
ck_session* ck_session_create(const char* bundle_dir, const char* config_json, ck_status* err);
void ck_session_destroy(ck_session* s);

ck_status ck_session_start(ck_session* s, int64_t t_ns);
ck_status ck_session_pause(ck_session* s, int64_t t_ns);
ck_status ck_session_resume(ck_session* s, int64_t t_ns);
ck_status ck_session_stop(ck_session* s, int64_t t_ns);  /* ends capture; final keyframe requests follow */
ck_status ck_session_finalize(ck_session* s);            /* closes streams, writes camera.json + manifest */
ck_state ck_session_state(const ck_session* s);
ck_status ck_session_pump(ck_session* s);                /* process queued input ("threaded":0 only) */

/* convention is an int32_t (a ck_pose_convention value): out-of-range values return CK_ERR_ARG
 * instead of being undefined behaviour as an enum. */
ck_status ck_ingest_pose(ck_session* s, const ck_pose* p, int32_t convention);
ck_status ck_ingest_gyro(ck_session* s, const ck_imu* m);
ck_status ck_ingest_accel(ck_session* s, const ck_imu* m);
ck_status ck_ingest_intrinsics(ck_session* s, const ck_intrinsics* k);
ck_status ck_ingest_frame(ck_session* s, const ck_frame* f);
ck_status ck_ingest_points(ck_session* s, int64_t t_ns, const float* xyz, size_t n_points);
ck_status ck_ingest_plane(ck_session* s, const ck_plane* p);
/* All boxes of one keyframe (schema 1.1.0 /ar/detections). n_boxes may be 0 ("ran, saw nothing") with
 * boxes NULL; more than 256 boxes returns CK_ERR_ARG. t_ns = the keyframe t_ns. Never blocks: a full
 * queue returns CK_ERR_FULL and is counted in INGEST_DROP ("detections"). */
ck_status ck_ingest_detections(ck_session* s, int64_t t_ns, uint32_t frame_id, const ck_detection* boxes,
                               size_t n_boxes);
ck_status ck_ingest_platform(ck_session* s, int64_t t_ns, const ck_platform* p);
ck_status ck_keyframe_written(ck_session* s, uint32_t frame_id, int32_t ok);

/* Copies whole JSON-line events (each ending in '\n') into buf; returns bytes written.
 * Returns 0 when none are pending or the next event does not fit (call again with a larger buffer). */
size_t ck_poll_events(ck_session* s, char* buf, size_t cap);
ck_status ck_policy_get(const ck_session* s, ck_policy* out);

/* Turns an interrupted bundle directory into a valid partial bundle (completion "recovered").
 * Writes a JSON summary into report (may be NULL). */
ck_status ck_recover(const char* bundle_dir, char* report, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* CAPTUREKIT_CAPI_H */
