// JNI bridge: dev.capturekit.nativebridge.CaptureKitNative <-> C API (capi/capturekit.h).
// Thin by design: argument unpacking only, no logic.
// Uses a handle map (small positive integers) instead of raw pointers to avoid
// ARM64 MTE/TBI tagged-pointer issues where reinterpret_cast<jlong>(ptr) can be
// negative, fooling the Kotlin "handle <= 0 means error" check.
#include <jni.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "capturekit/capi/capturekit.h"

#define FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_dev_capturekit_nativebridge_CaptureKitNative_##name

namespace {

// ---- Handle map: jlong <-> ck_session* ----
std::mutex g_mu;
std::unordered_map<jlong, ck_session*> g_handles;
jlong g_next = 1;

jlong add_handle(ck_session* s) {
  std::lock_guard<std::mutex> lk(g_mu);
  jlong h = g_next++;
  g_handles[h] = s;
  return h;  // always > 0
}

ck_session* get_handle(jlong h) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_handles.find(h);
  return it != g_handles.end() ? it->second : nullptr;
}

void remove_handle(jlong h) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_handles.erase(h);
}

#define S(h) get_handle(h)

std::string str(JNIEnv* e, jstring s) {
  if (!s) return {};
  const char* c = e->GetStringUTFChars(s, nullptr);
  std::string r(c ? c : "");
  if (c) e->ReleaseStringUTFChars(s, c);
  return r;
}
}  // namespace

FN(jstring, version)(JNIEnv* e, jobject) { return e->NewStringUTF(ck_version()); }

FN(jlong, create)(JNIEnv* e, jobject, jstring dir, jstring cfg) {
  ck_status err = CK_OK;
  auto d = str(e, dir), c = str(e, cfg);
  ck_session* s = ck_session_create(d.c_str(), c.empty() ? "{}" : c.c_str(), &err);
  return s ? add_handle(s) : -jlong(err);  // positive = valid handle, negative = ck_status error
}
FN(void, destroy)(JNIEnv*, jobject, jlong h) {
  ck_session* s = get_handle(h);
  if (s) { ck_session_destroy(s); remove_handle(h); }
}

FN(jint, start)(JNIEnv*, jobject, jlong h, jlong t) { return ck_session_start(S(h), t); }
FN(jint, pause)(JNIEnv*, jobject, jlong h, jlong t) { return ck_session_pause(S(h), t); }
FN(jint, resume)(JNIEnv*, jobject, jlong h, jlong t) { return ck_session_resume(S(h), t); }
FN(jint, stop)(JNIEnv*, jobject, jlong h, jlong t) { return ck_session_stop(S(h), t); }
FN(jint, finalizeSession)(JNIEnv*, jobject, jlong h) { return ck_session_finalize(S(h)); }
FN(jint, state)(JNIEnv*, jobject, jlong h) { return ck_session_state(S(h)); }

// pose convention fixed to ARKit/ARCore GL axes; q is [w,x,y,z] (Kotlin reorders ARCore x,y,z,w).
FN(jint, ingestPose)(JNIEnv*, jobject, jlong h, jlong t, jdouble tx, jdouble ty, jdouble tz, jdouble qw, jdouble qx,
                     jdouble qy, jdouble qz, jint state, jint reason) {
  ck_pose p{t, {tx, ty, tz}, {qw, qx, qy, qz}, state, reason};
  return ck_ingest_pose(S(h), &p, CK_POSE_ARKIT_GL);
}
FN(jint, ingestGyro)(JNIEnv*, jobject, jlong h, jlong t, jdouble x, jdouble y, jdouble z) {
  ck_imu m{t, {x, y, z}};
  return ck_ingest_gyro(S(h), &m);
}
FN(jint, ingestAccel)(JNIEnv*, jobject, jlong h, jlong t, jdouble x, jdouble y, jdouble z) {
  ck_imu m{t, {x, y, z}};
  return ck_ingest_accel(S(h), &m);
}
FN(jint, ingestIntrinsics)(JNIEnv*, jobject, jlong h, jlong t, jint w, jint ht, jdouble fx, jdouble fy, jdouble cx,
                           jdouble cy) {
  ck_intrinsics k{t, uint32_t(w), uint32_t(ht), fx, fy, cx, cy};
  return ck_ingest_intrinsics(S(h), &k);
}
// luma must be a direct ByteBuffer (the core copies it before returning).
FN(jint, ingestFrame)(JNIEnv* e, jobject, jlong h, jlong t, jint id, jlong exposure_ns, jobject luma, jint w, jint ht,
                      jint stride) {
  auto* p = static_cast<const uint8_t*>(e->GetDirectBufferAddress(luma));
  if (!p || e->GetDirectBufferCapacity(luma) < jlong(stride) * ht) return CK_ERR_ARG;
  ck_frame f{t, uint32_t(id), exposure_ns, p, w, ht, stride};
  return ck_ingest_frame(S(h), &f);
}
FN(jint, ingestPoints)(JNIEnv* e, jobject, jlong h, jlong t, jfloatArray xyz, jint n) {
  if (e->GetArrayLength(xyz) < 3 * n) return CK_ERR_ARG;
  std::vector<float> v(size_t(3 * n));
  e->GetFloatArrayRegion(xyz, 0, 3 * n, v.data());
  return ck_ingest_points(S(h), t, v.data(), size_t(n));
}
// d = center[3], normal[3], axis_u[3], extent_u, extent_v
FN(jint, ingestPlane)(JNIEnv* e, jobject, jlong h, jlong t, jstring id, jstring kind, jdoubleArray d) {
  if (e->GetArrayLength(d) < 11) return CK_ERR_ARG;
  double v[11];
  e->GetDoubleArrayRegion(d, 0, 11, v);
  auto sid = str(e, id), sk = str(e, kind);
  ck_plane p{t, sid.c_str(), sk.c_str(), {v[0], v[1], v[2]}, {v[3], v[4], v[5]}, {v[6], v[7], v[8]}, v[9], v[10]};
  return ck_ingest_plane(S(h), &p);
}
// boxes = flat [cls, score, x0, y0, x1, y1] * n (cls as float); n boxes, may be 0.
FN(jint, ingestDetections)(JNIEnv* e, jobject, jlong h, jlong t, jint frameId, jfloatArray boxes, jint n) {
  if (n < 0 || n > (1 << 16)) return CK_ERR_ARG;
  if (n > 0 && (boxes == nullptr || e->GetArrayLength(boxes) < 6 * n)) return CK_ERR_ARG;
  std::vector<float> v(size_t(6 * n));
  if (n) e->GetFloatArrayRegion(boxes, 0, 6 * n, v.data());
  std::vector<ck_detection> d;
  d.reserve(static_cast<size_t>(n));
  for (jint i = 0; i < n; ++i) {
    const float* b = &v[size_t(6) * size_t(i)];
    bool finite = true;
    for (int k = 0; k < 6; ++k) finite = finite && std::isfinite(b[k]);
    if (!finite) continue;  // skip boxes with NaN/Inf; the rest of the frame is still ingested
    // class id: clamp before the int32 conversion (float -> int out of range is UB)
    const float c = std::fmin(std::fmax(b[0], 0.0f), 1.0e6f);
    d.push_back(ck_detection{int32_t(c), b[1], b[2], b[3], b[4], b[5]});
  }
  return ck_ingest_detections(S(h), t, uint32_t(frameId), d.data(), d.size());
}
FN(jint, ingestPlatform)(JNIEnv*, jobject, jlong h, jlong t, jint thermal, jboolean lowPower, jdouble battery,
                         jboolean charging, jdouble frameTimeMs) {
  ck_platform p{thermal, lowPower ? 1 : 0, battery, charging ? 1 : 0, frameTimeMs};
  return ck_ingest_platform(S(h), t, &p);
}
FN(jint, keyframeWritten)(JNIEnv*, jobject, jlong h, jint id, jboolean ok) {
  return ck_keyframe_written(S(h), uint32_t(id), ok ? 1 : 0);
}
FN(jstring, pollEvents)(JNIEnv* e, jobject, jlong h) {
  std::vector<char> buf(1 << 16);
  size_t n = ck_poll_events(S(h), buf.data(), buf.size());
  return e->NewStringUTF(std::string(buf.data(), n).c_str());
}
// [level, keyframe_rate_scale, ml_rate_scale, tier, overlay, upload, must_pause]
FN(jdoubleArray, policy)(JNIEnv* e, jobject, jlong h) {
  ck_policy p{};
  ck_policy_get(S(h), &p);
  double v[7] = {double(p.level), p.keyframe_rate_scale, p.ml_rate_scale, double(p.capture_resolution_tier),
                 double(p.overlay_enabled), double(p.upload_allowed), double(p.must_pause)};
  jdoubleArray a = e->NewDoubleArray(7);
  e->SetDoubleArrayRegion(a, 0, 7, v);
  return a;
}
FN(jstring, recover)(JNIEnv* e, jobject, jstring dir) {
  char rep[1024];
  auto d = str(e, dir);
  ck_recover(d.c_str(), rep, sizeof rep);
  return e->NewStringUTF(rep);
}
