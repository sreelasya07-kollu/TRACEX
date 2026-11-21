#include "capturekit/bundle/validate.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_set>

#include "capturekit/util/json.hpp"

namespace fs = std::filesystem;

namespace capturekit::bundle {

std::string ValidationReport::verdict() const {
  bool warn = false;
  for (auto& i : issues) {
    if (i.severity == "ERROR") return "FAIL";
    warn |= i.severity == "WARN";
  }
  return warn ? "WARN" : "PASS";
}

bool ValidationReport::has(const std::string& code) const {
  return std::any_of(issues.begin(), issues.end(), [&](auto& i) { return i.code == code; });
}

std::string ValidationReport::to_json() const {
  std::vector<std::string> v;
  for (auto& i : issues) {
    json::Object o;
    o.set("code", i.code).set("severity", i.severity).set("level", i.level).set("message", i.message)
        .set("where", i.where);
    v.push_back(o.dump());
  }
  json::Object o;
  o.set("verdict", verdict()).set("level", "semantic").raw("issues", json::array(v.begin(), v.end()));
  return o.dump();
}

namespace {
bool finite(std::initializer_list<double> v) {
  return std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); });
}
// Paused intervals: SESSION_PAUSED until the next RESUMED/STOPPED/RECOVERED (or the end of the poses).
// Pose gaps while paused are not tracking loss. Computed once per validation.
std::vector<std::pair<int64_t, int64_t>> paused_intervals(const std::vector<EventRecord>& events, int64_t t_end) {
  std::vector<std::pair<int64_t, int64_t>> iv;
  std::vector<const EventRecord*> ev;
  for (auto& e : events) ev.push_back(&e);
  std::stable_sort(ev.begin(), ev.end(), [](auto* x, auto* y) { return x->t_ns < y->t_ns; });
  bool open = false;
  int64_t start = 0;
  for (auto* e : ev) {
    if (e->code == "SESSION_PAUSED") {
      if (!open) open = true, start = e->t_ns;
    } else if (open && (e->code == "SESSION_RESUMED" || e->code == "SESSION_STOPPED" || e->code == "SESSION_RECOVERED")) {
      iv.emplace_back(start, e->t_ns);
      open = false;
    }
  }
  if (open) iv.emplace_back(start, std::max(t_end, start));
  return iv;
}
// Nanoseconds of [a, b] inside the paused intervals.
int64_t paused_overlap(const std::vector<std::pair<int64_t, int64_t>>& iv, int64_t a, int64_t b) {
  int64_t sum = 0;
  for (auto& [s0, e0] : iv) sum += std::max<int64_t>(0, std::min(b, e0) - std::max(a, s0));
  return sum;
}
}  // namespace

ValidationReport validate_semantic(const std::string& dir, const StreamContents& s,
                                   const std::string& completion_status, const StreamCounts* counts) {
  ValidationReport r;
  auto add = [&](const char* code, const char* sev, std::string msg, std::string where = "") {
    r.issues.push_back({code, sev, "semantic", std::move(msg), std::move(where)});
  };

  {
    size_t zeros = 0;
    std::string first;
    for (auto& p : s.poses)
      if (p.t_ns <= 0 && !zeros++) first = "/pose t=" + std::to_string(p.t_ns);
    for (auto& k : s.intrinsics)
      if (k.t_ns <= 0 && !zeros++) first = "/intrinsics t=" + std::to_string(k.t_ns);
    if (zeros) add("TIME_ZERO", "ERROR", std::to_string(zeros) + " pose/intrinsics records with t_ns <= 0 (unmapped clock)", first);
  }
  size_t nan_poses = 0;
  int64_t nan_t = 0;
  size_t nonunit = 0;
  int64_t nonunit_t = 0;
  for (auto& p : s.poses) {
    const auto* q = p.q_world_from_camera_wxyz;
    const auto* t = p.t_world_from_camera;
    if (!finite({t[0], t[1], t[2], q[0], q[1], q[2], q[3]})) {
      if (!nan_poses++) nan_t = p.t_ns;
    } else if (std::fabs(std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) - 1) > kQuatTol) {
      if (!nonunit++) nonunit_t = p.t_ns;
    }
  }
  if (nan_poses) add("NAN_VALUE", "ERROR", std::to_string(nan_poses) + " poses with NaN/Inf", "/pose t=" + std::to_string(nan_t));
  for (auto [topic, v] : {std::pair<const char*, const std::vector<ImuSample>*>{"/imu/gyro", &s.gyro},
                          {"/imu/accel", &s.accel}}) {
    size_t n = 0;
    for (auto& m : *v) n += !finite({m.v[0], m.v[1], m.v[2]});
    if (n) add("NAN_VALUE", "ERROR", std::to_string(n) + " samples with NaN/Inf", topic);
  }
  if (nonunit) add("NON_UNIT_QUATERNION", "ERROR", std::to_string(nonunit) + " non-unit quaternions", "/pose t=" + std::to_string(nonunit_t));
  for (auto& k : s.intrinsics)
    if (!finite({k.fx, k.fy, k.cx, k.cy}) || k.fx <= 0) {
      add("NAN_VALUE", "ERROR", "invalid intrinsics", "/intrinsics");
      break;
    }

  std::vector<const FrameRecord*> kfs;
  for (auto& f : s.frames)
    if (f.keyframe) kfs.push_back(&f);
  if (!kfs.empty() && (s.intrinsics.empty() || s.intrinsics.front().t_ns > kfs.front()->t_ns))
    add("MISSING_INTRINSICS", "ERROR", "no intrinsics at or before first keyframe");

  std::vector<int64_t> pt;
  for (auto& p : s.poses) pt.push_back(p.t_ns);
  size_t no_pose = 0, limited = 0, missing = 0;
  int64_t no_pose_t = 0;
  std::string missing_first;
  for (auto* f : kfs) {
    auto it = std::lower_bound(pt.begin(), pt.end(), f->t_ns);
    bool exact = it != pt.end() && *it == f->t_ns;
    bool ok = exact || (it != pt.begin() && it != pt.end() && f->t_ns - *(it - 1) <= kKeyframePoseTolNs &&
                        *it - f->t_ns <= kKeyframePoseTolNs);
    if (!ok && !no_pose++) no_pose_t = f->t_ns;
    if (f->file.empty() || !fs::is_regular_file(fs::path(dir) / f->file)) {
      if (!missing++) missing_first = f->file.empty() ? "frame_id=" + std::to_string(f->frame_id) : f->file;
    }
    if (f->tracking_state != TrackingState::Unspecified && f->tracking_state != TrackingState::Normal) ++limited;
  }
  if (no_pose) add("KEYFRAME_WITHOUT_POSE", "ERROR", std::to_string(no_pose) + " keyframes lack bracketing poses", "t=" + std::to_string(no_pose_t));
  if (missing) add("KEYFRAME_FILE_MISSING", "ERROR", std::to_string(missing) + " keyframe files missing", missing_first);
  if (limited) add("KEYFRAME_TRACKING_LIMITED", "WARN", std::to_string(limited) + " keyframes while tracking not NORMAL");

  size_t gaps = 0;
  int64_t max_gap = 0;
  const int64_t t_last = pt.empty() ? 0 : pt.back();
  const auto paused = paused_intervals(s.events, t_last);
  for (size_t i = 1; i < pt.size(); ++i) {
    int64_t g = pt[i] - pt[i - 1];
    if (g > kPoseGapWarnNs) g -= paused_overlap(paused, pt[i - 1], pt[i]);
    if (g > kPoseGapWarnNs) ++gaps, max_gap = std::max(max_gap, g);
  }
  if (gaps) add("POSE_GAP", "WARN", std::to_string(gaps) + " gaps > 500 ms (max " + std::to_string(int64_t(std::llround(double(max_gap) / 1e6))) + " ms)");
  if (kfs.empty()) add("NO_KEYFRAMES", "ERROR", "the scan has no keyframes");
  else if (kfs.size() < kMinKeyframes) add("FEW_KEYFRAMES", "WARN", "only " + std::to_string(kfs.size()) + " keyframes");
  if (s.points.empty() && s.planes.empty()) add("NO_AR_GEOMETRY", "INFO", "no /ar/points or /ar/planes");

  if (!s.detections.empty()) {
    std::unordered_set<uint32_t> ids;
    for (auto& f : s.frames) ids.insert(f.frame_id);
    size_t bad = 0, unknown = 0;
    int64_t bad_t = 0, unknown_t = 0;
    for (auto& d : s.detections) {
      if (!ids.count(d.frame_id) && !unknown++) unknown_t = d.t_ns;
      for (auto& b : d.boxes) {
        bool ok = finite({b.score, b.x0, b.y0, b.x1, b.y1}) && b.x1 > b.x0 && b.y1 > b.y0 && b.score >= 0 && b.score <= 1;
        if (!ok && !bad++) bad_t = d.t_ns;
      }
    }
    if (bad) add("DETECTION_BAD_BOX", "WARN", std::to_string(bad) + " boxes non-finite, empty or with score outside [0,1]", "/ar/detections t=" + std::to_string(bad_t));
    if (unknown) add("DETECTION_UNKNOWN_FRAME", "INFO", std::to_string(unknown) + " detection records name a frame_id absent from /frame_index", "/ar/detections t=" + std::to_string(unknown_t));
  }

  if (!s.labels.empty() || !s.shell.empty() || !s.gaps.empty()) {  // schema 1.3.0 derived outputs (ADR 0028)
    size_t bad = 0, orphan = 0;
    int64_t bad_t = 0;
    auto unit = [](double v) { return std::isfinite(v) && v >= 0 && v <= 1; };
    auto mark = [&](bool ok, int64_t t) { if (!ok && !bad++) bad_t = t; };
    std::unordered_set<std::string> walls;
    for (auto& r : s.shell) {
      bool ok = std::isfinite(r.ceiling_h) && r.ceiling_h > 0 && r.corners.size() % 2 == 0;
      for (double c : r.corners) ok = ok && std::isfinite(c);
      for (auto& w : r.walls) {
        ok = ok && finite({w.x0, w.z0, w.x1, w.z1});
        walls.insert(w.id);
      }
      mark(ok, r.t_ns);
    }
    for (auto& r : s.labels) {
      for (auto& o : r.objects)
        mark(finite({o.centre[0], o.centre[1], o.centre[2], o.size[0], o.size[1], o.size[2], o.yaw}) && unit(o.conf) &&
                 o.size[0] > 0 && o.size[1] > 0 && o.size[2] > 0, r.t_ns);
      for (auto& o : r.openings) {
        mark(finite({o.x0, o.z0, o.x1, o.z1, o.y0, o.y1}) && unit(o.conf) && (o.kind == 0 || o.kind == 1), r.t_ns);
        if (!o.wall.empty() && !walls.count(o.wall)) ++orphan;
      }
    }
    for (auto& r : s.gaps)
      for (auto& g : r.gaps) mark(finite({g.x0, g.z0, g.x1, g.z1}) && unit(g.pri) && g.type >= 0 && g.type <= 5, r.t_ns);
    if (bad) add("GUIDE_OUTPUT_BAD_VALUE", "WARN", std::to_string(bad) + " /guide/* items non-finite, out of range or malformed", "/guide/* t=" + std::to_string(bad_t));
    if (orphan) add("LABEL_WALL_UNKNOWN", "INFO", std::to_string(orphan) + " openings name a wall id that no /guide/shell record lists", "/guide/labels");
  }

  if (counts) {
    const std::pair<const char*, uint64_t> actual[] = {{topic::kPose, s.poses.size()}, {topic::kGyro, s.gyro.size()},
                                                       {topic::kAccel, s.accel.size()}, {topic::kIntrinsics, s.intrinsics.size()},
                                                       {topic::kFrameIndex, s.frames.size()}, {topic::kEvents, s.events.size()}};
    std::string diff;
    for (auto& [topic, n] : actual)
      if (counts->get(topic) != n) diff += std::string(topic) + ": " + std::to_string(counts->get(topic)) + " != " + std::to_string(n) + "; ";
    if (counts->get(topic::kDetections) != s.detections.size())
      diff += std::string(topic::kDetections) + ": " + std::to_string(counts->get(topic::kDetections)) + " != " + std::to_string(s.detections.size()) + "; ";
    const std::pair<const char*, uint64_t> guide_actual[] = {{topic::kGuideLabels, s.labels.size()}, {topic::kGuideShell, s.shell.size()},
                                                              {topic::kGuideGaps, s.gaps.size()}};
    for (auto& [topic, n] : guide_actual)
      if (counts->get(topic) != n) diff += std::string(topic) + ": " + std::to_string(counts->get(topic)) + " != " + std::to_string(n) + "; ";
    if (counts->keyframes != kfs.size()) diff += "keyframes: " + std::to_string(counts->keyframes) + " != " + std::to_string(kfs.size()) + "; ";
    if (!diff.empty()) add("COUNT_MISMATCH", "WARN", diff);
  }
  if (completion_status == "partial" || completion_status == "recovered")
    add("PARTIAL_BUNDLE", "WARN", "completion.status = " + completion_status);
  return r;
}

}  // namespace capturekit::bundle
