#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include "capturekit/bundle/codec.hpp"
#include "capturekit/bundle/finalize.hpp"
#include "capturekit/bundle/stream_reader.hpp"
#include "capturekit/bundle/stream_writer.hpp"
#include "capturekit/bundle/validate.hpp"
#include "capturekit/util/json.hpp"
#include "capturekit/version.hpp"

namespace fs = std::filesystem;
using namespace capturekit::bundle;

namespace {
fs::path tmpdir(const std::string& name) {
  fs::path p = fs::path(CK_TEST_TMP) / name;
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

StreamCounts write_sample(const fs::path& file, int n_pose = 60) {
  StreamWriter w;
  StreamWriterOptions o;
  o.chunk_size = 2048;
  o.flush_interval_ns = 100'000'000;
  EXPECT_TRUE(w.open(file.string(), o));
  IntrinsicsSample k;
  k.t_ns = 1000; k.width = 640; k.height = 480; k.fx = 500; k.fy = 501; k.cx = 319.5; k.cy = 239.5;
  k.distortion = {0.01, -0.002, 0, 0};
  EXPECT_TRUE(w.write(k));
  for (int i = 0; i < n_pose; ++i) {
    int64_t t = 1000 + int64_t(i) * 33'333'333;
    PoseSample p;
    p.t_ns = t;
    p.t_world_from_camera[0] = 0.01 * i; p.t_world_from_camera[1] = 1.5; p.t_world_from_camera[2] = -0.5;
    double a = 0.01 * i;
    p.q_world_from_camera_wxyz[0] = std::cos(a); p.q_world_from_camera_wxyz[2] = std::sin(a);
    p.tracking_state = i < 3 ? TrackingState::Limited : TrackingState::Normal;
    p.tracking_reason = i < 3 ? TrackingReason::Initializing : TrackingReason::None;
    EXPECT_TRUE(w.write(p));
    for (int j = 0; j < 6; ++j) {
      ImuSample s; s.t_ns = t + j * 5'000'000; s.v[0] = 0.1 * j; s.v[1] = -9.8; s.v[2] = 0.0;
      EXPECT_TRUE(w.write_gyro(s));
      EXPECT_TRUE(w.write_accel(s));
    }
    if (i % 6 == 0) {
      FrameRecord f; f.t_ns = t + 1; f.frame_id = uint32_t(i / 6); f.keyframe = (i % 12 == 0);
      if (f.keyframe) f.file = "keyframes/" + std::to_string(i) + ".jpg";
      f.sharpness = 123.5; f.reasons = {"OK", ""}; f.tracking_state = TrackingState::Normal;
      EXPECT_TRUE(w.write(f));
    }
  }
  EventRecord e; e.t_ns = 5000; e.source = "session"; e.code = "SESSION_STATE"; e.message = "scanning";
  e.attrs = {{"from", "calibrating"}, {"to", "scanning"}};
  EXPECT_TRUE(w.write(e));
  PointCloudRecord pc; pc.t_ns = 6000; pc.xyz = {1.f, 2.f, 3.f, -4.f, 5.5f, 6.f};
  EXPECT_TRUE(w.write(pc));
  PlaneRecord pl; pl.t_ns = 7000; pl.id = "p1"; pl.kind = "floor"; pl.axis_u[0] = 0; pl.axis_u[2] = 1; pl.extent_u = 2; pl.extent_v = 3;
  EXPECT_TRUE(w.write(pl));
  DetectionsRecord dr; dr.t_ns = 8000; dr.frame_id = 2;
  dr.boxes = {{3, 0.875f, 10.5f, 20.25f, 100.f, 200.f}, {-1, 0.f, 0.f, 0.f, 1.f, 1.f}};  // zeros and a negative id survive
  EXPECT_TRUE(w.write(dr));
  DetectionsRecord none; none.t_ns = 9000; none.frame_id = 4;  // keyframe with no boxes
  EXPECT_TRUE(w.write(none));
  return w.close();
}
}  // namespace

TEST(Bundle, RoundTripCpp) {
  auto dir = tmpdir("rt");
  auto c = write_sample(dir / "streams.mcap");
  EXPECT_EQ(c.get(topic::kPose), 60u);
  EXPECT_EQ(c.get(topic::kGyro), 360u);
  EXPECT_EQ(c.keyframes, 5u);
  auto r = read_streams((dir / "streams.mcap").string());
  ASSERT_TRUE(r.opened);
  EXPECT_TRUE(r.finalized);
  EXPECT_EQ(r.undecodable, 0u);
  ASSERT_EQ(r.poses.size(), 60u);
  EXPECT_EQ(r.poses[0].tracking_state, TrackingState::Limited);
  EXPECT_EQ(r.poses[0].q_world_from_camera_wxyz[0], 1.0);
  EXPECT_DOUBLE_EQ(r.poses[59].t_world_from_camera[0], 0.59);
  EXPECT_DOUBLE_EQ(r.poses[59].q_world_from_camera_wxyz[2], std::sin(0.59));
  ASSERT_EQ(r.intrinsics.size(), 1u);
  EXPECT_EQ(r.intrinsics[0].distortion.size(), 4u);
  EXPECT_DOUBLE_EQ(r.intrinsics[0].fy, 501);
  ASSERT_EQ(r.frames.size(), 10u);
  EXPECT_EQ(r.frames[0].reasons.size(), 2u);
  ASSERT_EQ(r.events.size(), 1u);
  EXPECT_EQ(r.events[0].attrs[1].second, "scanning");
  ASSERT_EQ(r.points.size(), 1u);
  EXPECT_FLOAT_EQ(r.points[0].xyz[4], 5.5f);
  ASSERT_EQ(r.planes.size(), 1u);
  EXPECT_EQ(r.planes[0].axis_u[0], 0.0);  // zero survives proto3 omission
  EXPECT_EQ(r.planes[0].normal[1], 1.0);
  ASSERT_EQ(r.detections.size(), 2u);
  EXPECT_EQ(c.get(topic::kDetections), 2u);
  EXPECT_EQ(r.detections[0].frame_id, 2u);
  ASSERT_EQ(r.detections[0].boxes.size(), 2u);
  EXPECT_EQ(r.detections[0].boxes[0].cls, 3);
  EXPECT_EQ(r.detections[0].boxes[0].score, 0.875f);
  EXPECT_EQ(r.detections[0].boxes[0].x0, 10.5f);
  EXPECT_EQ(r.detections[0].boxes[0].y1, 200.f);
  EXPECT_EQ(r.detections[0].boxes[1].cls, -1);
  EXPECT_EQ(r.detections[0].boxes[1].x0, 0.f);
  EXPECT_EQ(r.detections[0].boxes[1].x1, 1.f);
  EXPECT_TRUE(r.detections[1].boxes.empty());
}

TEST(Bundle, DetectionsValidatorRules) {
  auto dir = tmpdir("det");
  write_sample(dir / "streams.mcap");
  auto r = read_streams((dir / "streams.mcap").string());
  auto ok = validate_semantic(dir.string(), r, "complete", nullptr);
  EXPECT_FALSE(ok.has("DETECTION_BAD_BOX"));
  EXPECT_FALSE(ok.has("DETECTION_UNKNOWN_FRAME"));
  r.detections[0].boxes.push_back({1, 0.5f, 50.f, 50.f, 40.f, 60.f});          // x1 <= x0
  r.detections[0].boxes.push_back({1, 1.5f, 1.f, 1.f, 4.f, 6.f});              // score > 1
  r.detections[1].boxes.push_back({1, std::nanf(""), 1.f, 1.f, 4.f, 6.f});     // NaN
  r.detections.push_back({10'000, 999, {}});                                    // frame_id not in /frame_index
  auto bad = validate_semantic(dir.string(), r, "complete", nullptr);
  ASSERT_TRUE(bad.has("DETECTION_BAD_BOX"));
  ASSERT_TRUE(bad.has("DETECTION_UNKNOWN_FRAME"));
  for (auto& i : bad.issues) {
    if (i.code == "DETECTION_BAD_BOX") { EXPECT_EQ(i.severity, "WARN"); EXPECT_NE(i.message.find("3 boxes"), std::string::npos); }
    if (i.code == "DETECTION_UNKNOWN_FRAME") { EXPECT_EQ(i.severity, "INFO"); EXPECT_NE(i.message.find("1 detection"), std::string::npos); }
  }
}

TEST(Bundle, RejectsNonMonotonic) {
  auto dir = tmpdir("mono");
  StreamWriter w;
  ASSERT_TRUE(w.open((dir / "s.mcap").string()));
  PoseSample p; p.t_ns = 10;
  EXPECT_TRUE(w.write(p));
  EXPECT_FALSE(w.write(p));
  p.t_ns = 5;
  EXPECT_FALSE(w.write(p));
  auto c = w.close();
  EXPECT_EQ(c.rejected_non_monotonic, 2u);
  EXPECT_EQ(c.get(topic::kPose), 1u);
}

TEST(Bundle, UnfinalizedFileIsRecoverable) {
  auto dir = tmpdir("unfin");
  auto path = (dir / "s.mcap").string();
  {
    StreamWriter w;
    StreamWriterOptions o; o.chunk_size = 1024; o.flush_interval_ns = 50'000'000;
    ASSERT_TRUE(w.open(path, o));
    for (int i = 0; i < 100; ++i) { PoseSample p; p.t_ns = 1 + int64_t(i) * 33'000'000; ASSERT_TRUE(w.write(p)); }
    w.flush();
    // Simulate a crash: copy the file before close() writes the summary.
    fs::copy_file(path, dir / "crashed.mcap");
  }
  auto r = read_streams((dir / "crashed.mcap").string());
  EXPECT_FALSE(r.finalized);
  EXPECT_EQ(r.poses.size(), 100u);
}

TEST(Bundle, TruncationFuzzNeverCrashes) {
  auto dir = tmpdir("fuzz");
  write_sample(dir / "full.mcap", 30);
  std::ifstream in(dir / "full.mcap", std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), {});
  std::mt19937 rng(42);
  size_t prev_poses = 0;
  for (size_t cut = 0; cut <= bytes.size(); cut += std::max<size_t>(1, bytes.size() / 400)) {
    auto p = dir / "cut.mcap";
    { std::ofstream o(p, std::ios::binary | std::ios::trunc); o.write(bytes.data(), std::streamsize(cut)); }
    auto r = read_streams(p.string());
    EXPECT_LE(r.poses.size(), 30u);
    if (cut == bytes.size()) EXPECT_TRUE(r.finalized);
    prev_poses = r.poses.size();
  }
  EXPECT_EQ(prev_poses, 30u);
  // Random byte corruption must not crash either.
  for (int k = 0; k < 200; ++k) {
    std::string b = bytes;
    for (int j = 0; j < 4; ++j) b[rng() % b.size()] = char(rng());
    auto p = dir / "bad.mcap";
    { std::ofstream o(p, std::ios::binary | std::ios::trunc); o.write(b.data(), std::streamsize(b.size())); }
    (void)read_streams(p.string());
  }
}

TEST(Bundle, CodecRejectsGarbage) {
  std::byte junk[] = {std::byte(0x0a), std::byte(0xff), std::byte(0xff)};
  EXPECT_FALSE(codec::decode_pose(junk, 3).has_value());
  EXPECT_FALSE(codec::decode_event(junk, 3).has_value());
  EXPECT_FALSE(codec::decode_detections(junk, 3).has_value());
  std::byte bad_box[] = {std::byte(0x1a), std::byte(0x05), std::byte(0x15), std::byte(0), std::byte(0)};  // boxes[0] with truncated float
  EXPECT_FALSE(codec::decode_detections(bad_box, 5).has_value());
}

TEST(Bundle, Sha256Vectors) {
  EXPECT_EQ(sha256_hex_bytes("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex_bytes("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::string m(1000000, 'a');
  EXPECT_EQ(sha256_hex_bytes(m.data(), m.size()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Bundle, ManifestAndCamera) {
  auto dir = tmpdir("manifest");
  auto c = write_sample(dir / "streams.mcap");
  fs::create_directories(dir / "keyframes");
  { std::ofstream(dir / "keyframes" / "000000.jpg") << "jpeg"; }
  BundleMeta m; m.bundle_id = "b1"; m.session_id = "s1"; m.model = "test \"quoted\"";
  IntrinsicsSample k; k.width = 640; k.height = 480; k.fx = k.fy = 500; k.cx = 319.5; k.cy = 239.5;
  ASSERT_TRUE(write_camera_json(dir.string(), k, m));
  auto files = list_files(dir.string());
  ASSERT_EQ(files.size(), 3u);
  EXPECT_EQ(files[0].path, "camera.json");
  EXPECT_EQ(files[1].role, "keyframe");
  EXPECT_EQ(files[1].sha256, sha256_hex_bytes("jpeg", 4));
  ASSERT_TRUE(write_manifest(dir.string(), m, c, files));
  EXPECT_TRUE(fs::exists(dir / "manifest.json"));
  EXPECT_FALSE(fs::exists(dir / "manifest.json.tmp"));
}

TEST(Bundle, ManifestConsentAndPrivacyBumpSchemaTo120) {
  auto dir = tmpdir("manifest_consent");
  auto c = write_sample(dir / "streams.mcap");
  fs::create_directories(dir / "privacy");
  { std::ofstream(dir / "privacy" / "redaction.json") << "{}"; }
  auto read = [&] {
    std::ifstream f(dir / "manifest.json");
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  BundleMeta m; m.bundle_id = "b1"; m.session_id = "s1";
  auto files = list_files(dir.string());
  bool has_role = false;
  for (auto& f : files) has_role |= f.path == "privacy/redaction.json" && f.role == "redaction";
  EXPECT_TRUE(has_role);
  // No consent, no redaction: still the 1.1.0 shape (older bundles and sim bundles are untouched).
  ASSERT_TRUE(write_manifest(dir.string(), m, c, files));
  auto plain = read();
  EXPECT_NE(plain.find("\"schema_version\": \"1.1.0\""), std::string::npos);
  EXPECT_EQ(plain.find("\"consent\""), std::string::npos);
  EXPECT_EQ(plain.find("\"privacy\""), std::string::npos);
  m.consent_version = "v1";
  m.consent_at = "2026-11-17T09:30:00Z";
  m.redaction_mode = "faces";
  ASSERT_TRUE(write_manifest(dir.string(), m, c, files));
  auto text = read();
  EXPECT_NE(text.find("\"schema_version\": \"1.2.0\""), std::string::npos);
  EXPECT_NE(text.find("\"consent\": {\"version\": \"v1\", \"at\": \"2026-11-17T09:30:00Z\"}"), std::string::npos);
  EXPECT_NE(text.find("\"privacy\": {\"redaction_mode\": \"faces\"}"), std::string::npos);
  EXPECT_NE(text.find("\"role\": \"redaction\""), std::string::npos);
}

// ---- schema 1.3.0: derived /guide/* channels (ADR 0028) ----

namespace {
LabelsRecord sample_labels(int64_t t) {
  LabelsRecord r; r.t_ns = t;
  LabelObject o; o.id = 7; o.cls = 2; o.centre[0] = 1.5; o.centre[1] = 0.45; o.centre[2] = -2.25;
  o.size[0] = 2.0; o.size[1] = 0.9; o.size[2] = 0.95; o.yaw = -0.5; o.conf = 0.8; o.n_obs = 4;
  r.objects.push_back(o);
  LabelOpening d; d.kind = 0; d.wall = "W2"; d.x0 = 3; d.z0 = -1; d.x1 = 3; d.z1 = 0; d.y0 = 0; d.y1 = 2.0; d.conf = 0.9;
  LabelOpening w; w.kind = 1; w.wall = ""; w.x0 = 0; w.z0 = 2; w.x1 = 1; w.z1 = 2; w.y0 = 0.9; w.y1 = 2.0; w.conf = 0.75;
  r.openings = {d, w};
  return r;
}
ShellRecord sample_shell(int64_t t) {
  ShellRecord r; r.t_ns = t;
  r.walls = {{"W1", 0, 0, 4, 0, false}, {"W2", 4, 0, 4, 3, true}};
  r.corners = {0, 0, 4, 0, 4, 3, 0, 3};
  r.closed = true; r.low_confidence = false; r.ceiling_h = 2.5; r.ceiling_inferred = true;
  return r;
}
GapsRecord sample_gaps(int64_t t) {
  GapsRecord r; r.t_ns = t;
  r.gaps = {{0, 4, 0, 4, 3, 0.75, "window wall"}, {4, 1, 1, 2, 2, 0.5, "behind the sofa"}, {1, 0, 0, 0, 0, 0, ""}};
  return r;
}
}  // namespace

TEST(Bundle, GuideOutputsRoundTripAndOldBundlesHaveNone) {
  auto dir = tmpdir("guide_out");
  {
    StreamWriter w;
    ASSERT_TRUE(w.open((dir / "streams.mcap").string()));
    IntrinsicsSample k; k.t_ns = 10; k.width = 640; k.height = 480; k.fx = k.fy = 500; k.cx = 319.5; k.cy = 239.5;
    ASSERT_TRUE(w.write(k));
    ASSERT_TRUE(w.write(sample_labels(100)));
    ASSERT_TRUE(w.write(sample_shell(100)));
    ASSERT_TRUE(w.write(sample_gaps(100)));
    EXPECT_FALSE(w.write(sample_gaps(100)));  // same t_ns: strictly increasing per channel
    ShellRecord empty; empty.t_ns = 200;      // empty record after content = withdrawn
    ASSERT_TRUE(w.write(empty));
    auto c = w.close();
    EXPECT_EQ(c.get(topic::kGuideLabels), 1u);
    EXPECT_EQ(c.get(topic::kGuideShell), 2u);
    EXPECT_EQ(c.get(topic::kGuideGaps), 1u);
  }
  auto r = read_streams((dir / "streams.mcap").string());
  ASSERT_TRUE(r.finalized);
  EXPECT_EQ(r.undecodable, 0u);
  ASSERT_EQ(r.labels.size(), 1u);
  ASSERT_EQ(r.labels[0].objects.size(), 1u);
  auto& o = r.labels[0].objects[0];
  EXPECT_EQ(o.id, 7u); EXPECT_EQ(o.cls, 2);
  EXPECT_DOUBLE_EQ(o.centre[2], -2.25); EXPECT_DOUBLE_EQ(o.size[1], 0.9); EXPECT_DOUBLE_EQ(o.yaw, -0.5);
  EXPECT_DOUBLE_EQ(o.conf, 0.8); EXPECT_EQ(o.n_obs, 4u);
  ASSERT_EQ(r.labels[0].openings.size(), 2u);
  EXPECT_EQ(r.labels[0].openings[0].wall, "W2");
  EXPECT_EQ(r.labels[0].openings[1].kind, 1);
  EXPECT_TRUE(r.labels[0].openings[1].wall.empty());
  EXPECT_DOUBLE_EQ(r.labels[0].openings[1].y0, 0.9);
  ASSERT_EQ(r.shell.size(), 2u);
  ASSERT_EQ(r.shell[0].walls.size(), 2u);
  EXPECT_EQ(r.shell[0].walls[1].id, "W2");
  EXPECT_TRUE(r.shell[0].walls[1].inferred);
  EXPECT_FALSE(r.shell[0].walls[0].inferred);
  EXPECT_EQ(r.shell[0].corners, (std::vector<double>{0, 0, 4, 0, 4, 3, 0, 3}));
  EXPECT_TRUE(r.shell[0].closed); EXPECT_FALSE(r.shell[0].low_confidence);
  EXPECT_DOUBLE_EQ(r.shell[0].ceiling_h, 2.5); EXPECT_TRUE(r.shell[0].ceiling_inferred);
  EXPECT_TRUE(r.shell[1].walls.empty()); EXPECT_TRUE(r.shell[1].corners.empty());
  ASSERT_EQ(r.gaps.size(), 1u);
  ASSERT_EQ(r.gaps[0].gaps.size(), 3u);
  EXPECT_EQ(r.gaps[0].gaps[1].type, 4);
  EXPECT_EQ(r.gaps[0].gaps[1].why, "behind the sofa");
  EXPECT_DOUBLE_EQ(r.gaps[0].gaps[0].pri, 0.75);
  EXPECT_EQ(r.gaps[0].gaps[2].type, 1);  // all-zero gap survives

  // A stream written without the channels (every pre-1.3.0 bundle) reads back empty.
  auto old = tmpdir("guide_out_old");
  write_sample(old / "streams.mcap");
  auto ro = read_streams((old / "streams.mcap").string());
  EXPECT_TRUE(ro.labels.empty() && ro.shell.empty() && ro.gaps.empty());
  EXPECT_EQ(ro.undecodable, 0u);
}

TEST(Bundle, GuideOutputsValidatorRules) {
  auto dir = tmpdir("guide_val");
  write_sample(dir / "streams.mcap");
  auto r = read_streams((dir / "streams.mcap").string());
  r.labels.push_back(sample_labels(100));
  r.shell.push_back(sample_shell(100));
  r.gaps.push_back(sample_gaps(100));
  auto ok = validate_semantic(dir.string(), r, "complete", nullptr);
  EXPECT_FALSE(ok.has("GUIDE_OUTPUT_BAD_VALUE"));
  EXPECT_FALSE(ok.has("LABEL_WALL_UNKNOWN"));  // W2 is listed by the shell
  r.labels[0].openings[0].wall = "W9";
  auto orphan = validate_semantic(dir.string(), r, "complete", nullptr);
  ASSERT_TRUE(orphan.has("LABEL_WALL_UNKNOWN"));
  for (auto& i : orphan.issues) if (i.code == "LABEL_WALL_UNKNOWN") EXPECT_EQ(i.severity, "INFO");
  r.labels[0].openings[0].wall = "W2";
  r.labels[0].objects[0].conf = 1.5;                         // out of range
  r.labels[0].objects[0].size[1] = 0;                        // empty box
  r.shell[0].corners.push_back(1.0);                         // odd count
  r.gaps[0].gaps[0].pri = std::nan("");                      // NaN
  r.gaps[0].gaps[1].type = 9;                                // unknown type
  auto bad = validate_semantic(dir.string(), r, "complete", nullptr);
  ASSERT_TRUE(bad.has("GUIDE_OUTPUT_BAD_VALUE"));
  for (auto& i : bad.issues)
    if (i.code == "GUIDE_OUTPUT_BAD_VALUE") { EXPECT_EQ(i.severity, "WARN"); EXPECT_NE(i.message.find("4 /guide"), std::string::npos) << i.message; }
}

TEST(Bundle, ManifestGuideOutputsBumpSchemaTo130) {
  auto dir = tmpdir("manifest_guide");
  auto c = write_sample(dir / "streams.mcap");
  auto read = [&] {
    std::ifstream f(dir / "manifest.json");
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  BundleMeta m; m.bundle_id = "b1"; m.session_id = "s1";
  auto files = list_files(dir.string());
  ASSERT_TRUE(write_manifest(dir.string(), m, c, files));
  auto plain = read();  // no output records: shape unchanged
  EXPECT_NE(plain.find("\"schema_version\": \"1.1.0\""), std::string::npos);
  EXPECT_EQ(plain.find("guide_"), std::string::npos);
  c.per_topic[topic::kGuideShell] = 2;
  ASSERT_TRUE(write_manifest(dir.string(), m, c, files));
  auto text = read();
  EXPECT_NE(text.find("\"schema_version\": \"1.3.0\""), std::string::npos);
  EXPECT_NE(text.find("\"guide_labels\": 0, \"guide_shell\": 2, \"guide_gaps\": 0"), std::string::npos);
  m.consent_version = "v1"; m.consent_at = "2026-11-18T09:30:00Z";  // consent keeps 1.3.0, never lowers it
  ASSERT_TRUE(write_manifest(dir.string(), m, c, files));
  EXPECT_NE(read().find("\"schema_version\": \"1.3.0\""), std::string::npos);
}

TEST(Bundle, ShellRaggedPackedCornersAreMalformed) {
  auto bytes = [](std::initializer_list<int> v) {
    std::vector<std::byte> b;
    for (int x : v) b.push_back(std::byte(x));
    return b;
  };
  auto ok = bytes({0x1A, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});  // field 3, 16 bytes = 2 doubles
  auto d = codec::decode_shell(ok.data(), ok.size());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->corners.size(), 2u);
  auto ragged = bytes({0x1A, 5, 0, 0, 0, 0, 0});  // 5 bytes: not a multiple of 8
  EXPECT_FALSE(codec::decode_shell(ragged.data(), ragged.size()).has_value());
}

TEST(Json, FlatParse) {
  auto o = capturekit::json::parse_flat(R"({"a": 1.5, "b": "x\"y", "c": true, "d": -2e-3})");
  ASSERT_TRUE(o);
  EXPECT_DOUBLE_EQ(std::get<double>(o->at("a")), 1.5);
  EXPECT_EQ(std::get<std::string>(o->at("b")), "x\"y");
  EXPECT_TRUE(std::get<bool>(o->at("c")));
  EXPECT_DOUBLE_EQ(std::get<double>(o->at("d")), -0.002);
  std::string err;
  EXPECT_FALSE(capturekit::json::parse_flat(R"({"a": {"b": 1}})", &err));
  EXPECT_FALSE(err.empty());
  EXPECT_FALSE(capturekit::json::parse_flat(R"({"a": 1} x)"));
}

TEST(Version, Core) { EXPECT_STREQ(capturekit::core_version(), "0.0.1"); }
