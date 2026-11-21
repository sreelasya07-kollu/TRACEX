// C API implementation: thin translation onto capturekit::session::Session.
#include <cstring>
#include <string>

#include "capturekit/capi/capturekit.h"
#include "capturekit/geometry/geometry.hpp"
#include "capturekit/session/session.hpp"
#include "capturekit/version.hpp"

using namespace capturekit;

struct ck_session {
  std::unique_ptr<session::Session> s;
};

namespace {
ck_status st(session::Status s) { return ck_status(int(s)); }

template <class F>
ck_status guard(ck_session* s, F&& f) {
  if (!s || !s->s) return CK_ERR_ARG;
  try {
    return st(f(*s->s));
  } catch (...) {
    return CK_ERR_IO;
  }
}
}  // namespace

extern "C" {

const char* ck_version(void) { return core_version(); }

ck_session* ck_session_create(const char* dir, const char* cfg_json, ck_status* err) {
  auto fail = [&](ck_status e) -> ck_session* {
    if (err) *err = e;
    return nullptr;
  };
  if (!dir) return fail(CK_ERR_ARG);
  std::string e;
  auto cfg = session::SessionConfig::from_json(cfg_json ? cfg_json : "{}", &e);
  if (!cfg) return fail(CK_ERR_ARG);
  try {
    auto s = session::Session::create(dir, *cfg, &e);
    if (!s) return fail(CK_ERR_IO);
    if (err) *err = CK_OK;
    return new ck_session{std::move(s)};
  } catch (...) {
    return fail(CK_ERR_IO);
  }
}

void ck_session_destroy(ck_session* s) { delete s; }

ck_status ck_session_start(ck_session* s, int64_t t) { return guard(s, [&](auto& x) { return x.start(t); }); }
ck_status ck_session_pause(ck_session* s, int64_t t) { return guard(s, [&](auto& x) { return x.pause(t); }); }
ck_status ck_session_resume(ck_session* s, int64_t t) { return guard(s, [&](auto& x) { return x.resume(t); }); }
ck_status ck_session_stop(ck_session* s, int64_t t) { return guard(s, [&](auto& x) { return x.stop(t); }); }
ck_status ck_session_finalize(ck_session* s) { return guard(s, [&](auto& x) { return x.finalize(); }); }
ck_status ck_session_pump(ck_session* s) { return guard(s, [&](auto& x) { return x.pump(); }); }

ck_state ck_session_state(const ck_session* s) {
  return (s && s->s) ? ck_state(int(s->s->state())) : CK_STATE_FAILED;
}

ck_status ck_ingest_pose(ck_session* s, const ck_pose* p, int32_t conv) {
  if (!p) return CK_ERR_ARG;
  return guard(s, [&](auto& x) {
    bundle::PoseSample ps;
    ps.t_ns = p->t_ns;
    ps.tracking_state = bundle::TrackingState(p->tracking_state);
    ps.tracking_reason = bundle::TrackingReason(p->tracking_reason);
    if (conv == CK_POSE_ARKIT_GL) {
      auto T = geometry::from_arkit(geometry::PoseWorldFromGlCamera::from_tq(p->t, p->q_wxyz));
      T.to_tq(ps.t_world_from_camera, ps.q_world_from_camera_wxyz);
    } else if (conv == CK_POSE_OPENCV) {
      geometry::PoseWC::from_tq(p->t, p->q_wxyz).to_tq(ps.t_world_from_camera, ps.q_world_from_camera_wxyz);
    } else {
      return session::Status::ErrArg;
    }
    return x.ingest_pose(ps);
  });
}

ck_status ck_ingest_gyro(ck_session* s, const ck_imu* m) {
  if (!m) return CK_ERR_ARG;
  return guard(s, [&](auto& x) {
    bundle::ImuSample i;
    i.t_ns = m->t_ns;
    std::memcpy(i.v, m->v, sizeof i.v);
    return x.ingest_gyro(i);
  });
}

ck_status ck_ingest_accel(ck_session* s, const ck_imu* m) {
  if (!m) return CK_ERR_ARG;
  return guard(s, [&](auto& x) {
    bundle::ImuSample i;
    i.t_ns = m->t_ns;
    std::memcpy(i.v, m->v, sizeof i.v);
    return x.ingest_accel(i);
  });
}

ck_status ck_ingest_intrinsics(ck_session* s, const ck_intrinsics* k) {
  if (!k) return CK_ERR_ARG;
  return guard(s, [&](auto& x) {
    bundle::IntrinsicsSample i;
    i.t_ns = k->t_ns;
    i.width = k->width;
    i.height = k->height;
    i.fx = k->fx;
    i.fy = k->fy;
    i.cx = k->cx;
    i.cy = k->cy;
    return x.ingest_intrinsics(i);
  });
}

ck_status ck_ingest_frame(ck_session* s, const ck_frame* f) {
  if (!f) return CK_ERR_ARG;
  return guard(s, [&](auto& x) {
    session::FrameInput in{f->t_ns, f->frame_id, f->exposure_ns, f->luma, f->width, f->height, f->stride};
    return x.ingest_frame(in);
  });
}

ck_status ck_ingest_points(ck_session* s, int64_t t, const float* xyz, size_t n) {
  if (!xyz && n) return CK_ERR_ARG;
  return guard(s, [&](auto& x) {
    bundle::PointCloudRecord p;
    p.t_ns = t;
    p.xyz.assign(xyz, xyz + 3 * n);
    return x.ingest_points(p);
  });
}

ck_status ck_ingest_plane(ck_session* s, const ck_plane* p) {
  if (!p || !p->id) return CK_ERR_ARG;
  return guard(s, [&](auto& x) {
    bundle::PlaneRecord r;
    r.t_ns = p->t_ns;
    r.id = p->id;
    r.kind = p->kind ? p->kind : "other";
    std::memcpy(r.center, p->center, sizeof r.center);
    std::memcpy(r.normal, p->normal, sizeof r.normal);
    std::memcpy(r.axis_u, p->axis_u, sizeof r.axis_u);
    r.extent_u = p->extent_u;
    r.extent_v = p->extent_v;
    return x.ingest_plane(r);
  });
}

ck_status ck_ingest_detections(ck_session* s, int64_t t, uint32_t frame_id, const ck_detection* boxes, size_t n) {
  if (!boxes && n) return CK_ERR_ARG;
  if (n > session::Session::kMaxDetectionBoxes) return CK_ERR_ARG;
  return guard(s, [&](auto& x) {
    bundle::DetectionsRecord r;
    r.t_ns = t;
    r.frame_id = frame_id;
    r.boxes.reserve(n);
    for (size_t i = 0; i < n; ++i) r.boxes.push_back({boxes[i].cls, boxes[i].score, boxes[i].x0, boxes[i].y0, boxes[i].x1, boxes[i].y1});
    return x.ingest_detections(r);
  });
}

ck_status ck_ingest_platform(ck_session* s, int64_t t, const ck_platform* p) {
  if (!p) return CK_ERR_ARG;
  return guard(s, [&](auto& x) {
    governor::Signals g;
    g.t_ns = t;
    g.thermal_level = p->thermal_level;
    g.low_power_mode = p->low_power_mode != 0;
    g.battery_pct = p->battery_pct;
    g.charging = p->charging != 0;
    g.frame_time_ms_ema = p->frame_time_ms;
    return x.ingest_platform(t, g);
  });
}

ck_status ck_keyframe_written(ck_session* s, uint32_t id, int32_t ok) {
  return guard(s, [&](auto& x) { return x.keyframe_written(id, ok != 0); });
}

size_t ck_poll_events(ck_session* s, char* buf, size_t cap) {
  if (!s || !s->s || !buf || cap == 0) return 0;
  auto out = s->s->poll_events(cap);
  std::memcpy(buf, out.data(), out.size());
  return out.size();
}

ck_status ck_policy_get(const ck_session* s, ck_policy* out) {
  if (!s || !s->s || !out) return CK_ERR_ARG;
  auto p = s->s->policy();
  out->level = int32_t(p.level);
  out->keyframe_rate_scale = p.keyframe_rate_scale;
  out->ml_rate_scale = p.ml_rate_scale;
  out->capture_resolution_tier = p.capture_resolution_tier;
  out->overlay_enabled = p.overlay_enabled;
  out->upload_allowed = p.upload_allowed;
  out->must_pause = p.must_pause;
  return CK_OK;
}

ck_status ck_recover(const char* dir, char* report, size_t cap) {
  if (!dir) return CK_ERR_ARG;
  session::RecoverResult r;
  try {
    r = session::recover(dir);
  } catch (...) {
    r.error = "exception during recovery";
  }
  if (report && cap) {
    auto j = r.to_json();
    size_t n = std::min(j.size(), cap - 1);
    std::memcpy(report, j.data(), n);
    report[n] = '\0';
  }
  return r.ok ? CK_OK : CK_ERR_IO;
}

}  // extern "C"
