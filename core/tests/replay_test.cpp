// Phase 3: replay sinks.
#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "capturekit/guide/engine.hpp"
#include "capturekit/guide/guide_sink.hpp"
#include "capturekit/replay/metrics_sink.hpp"

using namespace capturekit;
using namespace capturekit::replay;

namespace {
bundle::PoseSample pose(int64_t t, double x, bundle::TrackingState s = bundle::TrackingState::Normal) {
  bundle::PoseSample p;
  p.t_ns = t;
  p.t_world_from_camera[0] = x;
  p.tracking_state = s;
  return p;
}

void feed(ReplaySink& s) {
  const int64_t ms = 1'000'000;
  for (int64_t i = 0; i < 100; ++i) {
    auto st = (i >= 40 && i < 60) ? bundle::TrackingState::NotAvailable : bundle::TrackingState::Normal;
    s.on_pose(pose(i * 33 * ms, 0.01 * double(i), st));
    bundle::ImuSample g;
    g.t_ns = i * 33 * ms + 1;
    s.on_gyro(g);
  }
  bundle::ImuSample late;
  late.t_ns = 5 * ms;  // non-monotonic
  s.on_gyro(late);
  bundle::EventRecord e;
  e.t_ns = 10 * ms;
  e.source = "platform";
  e.code = "thermal";
  s.on_event(e);
  s.on_end(99 * 33 * ms);
}
}  // namespace

TEST(Replay, MetricsSinkTrackingIntervalsAndRejects) {
  MetricsSink m;
  feed(m);
  ASSERT_EQ(m.tracking_intervals().size(), 1u);
  EXPECT_EQ(m.tracking_intervals()[0].t0_ns, 40 * 33 * 1'000'000LL);
  EXPECT_EQ(m.tracking_intervals()[0].t1_ns, 60 * 33 * 1'000'000LL);
  EXPECT_EQ(m.rejected("gyro"), 1u);
  auto j = m.report_json();
  EXPECT_NE(j.find("\"platform.thermal\":1"), std::string::npos) << j;
  EXPECT_NE(j.find("\"rejected_non_monotonic\":1"), std::string::npos) << j;
}

TEST(Replay, ReportIsDeterministic) {
  auto a = make_sink("metrics", "{}");
  auto b = make_sink("metrics", "{}");
  feed(*a);
  feed(*b);
  EXPECT_EQ(a->report_json(), b->report_json());
}

TEST(Replay, DetectionsDeliveredAndDeterministic) {
  auto run = [] {
    auto s = make_sink("metrics", "{}");
    feed(*s);
    return s;
  };
  auto base = run()->report_json();
  EXPECT_EQ(base.find("detections"), std::string::npos) << "no detections -> report unchanged";
  auto with = [] {
    auto s = make_sink("metrics", "{}");
    bundle::DetectionsRecord d; d.frame_id = 3; d.boxes = {{2, 0.5f, 1, 2, 30, 40}};
    for (int64_t i = 1; i <= 3; ++i) { d.t_ns = i * 1'000'000'000LL; s->on_detections(d); }
    d.t_ns = 3'000'000'000LL;  // duplicate t_ns: rejected as non-monotonic, like other channels
    s->on_detections(d);
    feed(*s);
    return s->report_json();
  };
  auto a = with(), b = with();
  EXPECT_EQ(a, b);
  EXPECT_NE(a.find("\"detections\":{"), std::string::npos) << a;
  MetricsSink m;
  bundle::DetectionsRecord d; d.t_ns = 5; m.on_detections(d); m.on_detections(d);
  EXPECT_EQ(m.rejected("detections"), 1u);
}

TEST(Replay, OpenIntervalClosedAtEnd) {
  MetricsSink m;
  m.on_pose(pose(0, 0));
  m.on_pose(pose(10, 0, bundle::TrackingState::Limited));
  m.on_end(50);
  ASSERT_EQ(m.tracking_intervals().size(), 1u);
  EXPECT_EQ(m.tracking_intervals()[0].t1_ns, 50);
}

TEST(Replay, UnknownSinkThrows) { EXPECT_THROW(make_sink("nope", "{}"), std::invalid_argument); }

// Phase 9 step 5: the guide sink consumes recorded detections exactly like the live engine.
namespace {
bundle::PlaneRecord rplane(const char* id, const char* kind, std::array<double, 3> c, std::array<double, 3> n, double eu, double ev) {
  bundle::PlaneRecord p;
  p.id = id, p.kind = kind;
  for (size_t i = 0; i < 3; ++i) p.center[i] = c[i], p.normal[i] = n[i];
  p.extent_u = eu, p.extent_v = ev;
  return p;
}

bundle::IntrinsicsSample guide_k() {
  bundle::IntrinsicsSample k;
  k.width = 640, k.height = 480, k.fx = k.fy = 500, k.cx = 319.5, k.cy = 239.5;
  return k;
}

// Camera at (2.5, 1.4, 0) looking along +Z, pitched 10 degrees down.
geometry::PoseWC guide_T() { return guide::pose_from_yaw_pitch({2.5, 1.4, 0.0}, 0.0, -10.0 * 3.14159265358979323846 / 180.0); }

bundle::PoseSample guide_pose(int64_t t) {
  auto T = guide_T();
  bundle::PoseSample s;
  s.t_ns = t;
  for (int i = 0; i < 3; ++i) s.t_world_from_camera[i] = T.translation()[i];
  auto q = T.se3().unit_quaternion();
  s.q_world_from_camera_wxyz[0] = q.w(), s.q_world_from_camera_wxyz[1] = q.x();
  s.q_world_from_camera_wxyz[2] = q.y(), s.q_world_from_camera_wxyz[3] = q.z();
  return s;
}

// One chair (0.5 x 0.9 x 0.5 m) 3 m ahead on the floor, as a pixel box.
bundle::DetectionsRecord guide_dets(int frame) {
  geometry::Intrinsics K;
  K.width = 640, K.height = 480, K.fx = K.fy = 500, K.cx = 319.5, K.cy = 239.5;
  double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (int i = 0; i < 8; ++i) {
    Eigen::Vector3d c(2.5 + (i & 1 ? 0.25 : -0.25), (i & 2) ? 0.9 : 0.0, 3.0 + (i & 4 ? 0.25 : -0.25));
    auto px = geometry::project(K, guide_T(), c);
    x0 = std::min(x0, (*px)[0]), x1 = std::max(x1, (*px)[0]), y0 = std::min(y0, (*px)[1]), y1 = std::max(y1, (*px)[1]);
  }
  bundle::DetectionsRecord r;
  r.t_ns = int64_t(frame) * 100'000'000, r.frame_id = uint32_t(frame);
  r.boxes.push_back({1, 0.8f, float(x0), float(y0), float(x1), float(y1)});  // class 1 = chair
  return r;
}

void feed_guide(replay::ReplaySink& s, bool detections) {
  s.on_intrinsics(guide_k());
  s.on_plane(rplane("f", "floor", {2.5, 0, 2}, {0, 1, 0}, 6, 6));
  s.on_plane(rplane("w1", "wall", {2.5, 1.25, 3.5}, {0, 0, -1}, 5, 2.5));
  for (int64_t i = 0; i <= 20; ++i) {
    s.on_pose(guide_pose(i * 50'000'000));
    if (detections && i % 2 == 0 && i >= 2 && i <= 6) s.on_detections(guide_dets(int(i / 2)));  // frames 1..3
  }
  s.on_end(1'000'000'000);
}
}  // namespace

TEST(Replay, GuideSinkLiftsRecordedDetectionsLikeTheLiveEngine) {
  auto run = [](const char* cfg, bool detections) {
    auto s = make_sink("guide", cfg);
    feed_guide(*s, detections);
    return s->report_json();
  };
  const auto a = run("{\"understanding\":1}", true), b = run("{\"understanding\":1}", true);
  EXPECT_EQ(a, b);  // replay twice: byte-identical
  EXPECT_NE(a.find("\"objects\":[{"), std::string::npos) << a;
  EXPECT_NE(a.find("\"cls\":\"chair\""), std::string::npos) << a;
  // Live engine fed the same calls: same engine report as the sink's.
  guide::GuideParams gp;
  gp.understanding = 1;
  guide::GuideEngine live(gp);
  live.on_intrinsics(guide_k());
  live.on_plane(rplane("f", "floor", {2.5, 0, 2}, {0, 1, 0}, 6, 6));
  live.on_plane(rplane("w1", "wall", {2.5, 1.25, 3.5}, {0, 0, -1}, 5, 2.5));
  for (int64_t i = 0; i <= 20; ++i) {
    live.on_pose(guide_pose(i * 50'000'000));
    if (i % 2 == 0 && i >= 2 && i <= 6) live.on_detections(guide_dets(int(i / 2)));
  }
  ASSERT_EQ(live.objects().size(), 1u);
  EXPECT_NE(a.find(live.report_json().substr(live.report_json().find("\"objects\":[{"), 40)), std::string::npos);
  // Flag off: recorded detections are inert; the report equals the one without any detections.
  EXPECT_EQ(run("{}", true), run("{}", false));
  EXPECT_EQ(run("{}", true).find("\"objects\""), std::string::npos);
  // Flag on but the stream has no detections: no objects key either.
  EXPECT_EQ(run("{\"understanding\":1}", false).find("\"objects\""), std::string::npos);
}

// A detection stamped off the pose grid (pose + 10 ms, past the newest pose) still lifts, identically live and offline.
TEST(Replay, GuideSinkOffGridDetectionTimeAgreesWithTheLiveEngine) {
  auto sink = guide::GuideSink::create("{\"understanding\":1}");
  guide::GuideParams gp;
  gp.understanding = 1;
  guide::GuideEngine live(gp);
  std::vector<guide::Instruction> sink_out;  // the sink also runs update() per pose; do the same for the live side
  struct Live {
    guide::GuideEngine& e;
    std::vector<guide::Instruction>& out;
    void on_intrinsics(const bundle::IntrinsicsSample& k) { e.on_intrinsics(k); }
    void on_plane(const bundle::PlaneRecord& p) { e.on_plane(p); }
    void on_pose(const bundle::PoseSample& p) { e.on_pose(p), e.update(p.t_ns, out); }
    void on_detections(const bundle::DetectionsRecord& d) { e.on_detections(d); }
  } live_feed{live, sink_out};
  auto feed = [&](auto& s) {
    s.on_intrinsics(guide_k());
    s.on_plane(rplane("f", "floor", {2.5, 0, 2}, {0, 1, 0}, 6, 6));
    s.on_plane(rplane("w1", "wall", {2.5, 1.25, 3.5}, {0, 0, -1}, 5, 2.5));
    for (int64_t i = 0; i <= 20; ++i) s.on_pose(guide_pose(i * 50'000'000));
    for (int f = 1; f <= 3; ++f) {
      auto d = guide_dets(f);
      d.t_ns = 1'000'000'000 + 10'000'000LL * f;  // 10, 20, 30 ms after the newest pose
      s.on_detections(d);
    }
  };
  feed(*sink);
  feed(live_feed);
  ASSERT_EQ(live.objects().size(), 1u);
  auto& g = static_cast<guide::GuideSink&>(*sink);
  EXPECT_EQ(g.engine().objects().size(), 1u);
  EXPECT_EQ(g.engine().report_json(), live.report_json());
  EXPECT_EQ(g.engine().overlay(1'000'000'000).to_json(), live.overlay(1'000'000'000).to_json());
}
