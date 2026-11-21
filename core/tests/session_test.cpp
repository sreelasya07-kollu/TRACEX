// Phase 6: C API session state machine, bounded queues, keyframe handshake, governor pause,
// threaded ingest stress, crash recovery.
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "capturekit/bundle/codec.hpp"
#include "capturekit/bundle/stream_reader.hpp"
#include "capturekit/bundle/stream_writer.hpp"
#include "capturekit/capi/capturekit.h"
#include "capturekit/session/session.hpp"

namespace fs = std::filesystem;
using namespace capturekit;

namespace {
constexpr int64_t kMs = 1'000'000;

fs::path fresh(const std::string& name) {
  fs::path d = fs::path(CK_TEST_TMP) / "session" / name;
  fs::remove_all(d);
  fs::create_directories(d.parent_path());
  return d;
}

std::vector<uint8_t> texture(int w, int h, uint32_t seed) {
  std::vector<uint8_t> px(size_t(w * h), 128);
  std::mt19937 g(seed);
  for (int k = 0; k < 300; ++k) {
    int x0 = int(g() % uint32_t(w)), y0 = int(g() % uint32_t(h)), c = 30 + int(g() % 190);
    for (int y = y0; y < std::min(h, y0 + 16); ++y)
      for (int x = x0; x < std::min(w, x0 + 16); ++x) px[size_t(y * w + x)] = uint8_t(c);
  }
  return px;
}

void write_jpeg(const fs::path& p, bool complete = true) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  const unsigned char head[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 'J', 'F', 'I', 'F'};
  f.write(reinterpret_cast<const char*>(head), sizeof head);
  if (complete) {
    const unsigned char tail[] = {0xFF, 0xD9};
    f.write(reinterpret_cast<const char*>(tail), 2);
  }
}

std::vector<std::string> drain(ck_session* s) {
  std::vector<std::string> out;
  char buf[1 << 16];
  for (size_t n; (n = ck_poll_events(s, buf, sizeof buf)) > 0;) {
    std::string chunk(buf, n);
    size_t a = 0;
    for (size_t b; (b = chunk.find('\n', a)) != std::string::npos; a = b + 1) out.push_back(chunk.substr(a, b - a));
  }
  return out;
}

uint32_t json_u32(const std::string& line, const std::string& key) {
  auto p = line.find("\"" + key + "\":");
  return uint32_t(std::stoul(line.substr(p + key.size() + 3)));
}
std::string json_str(const std::string& line, const std::string& key) {
  auto p = line.find("\"" + key + "\":\"") + key.size() + 4;
  return line.substr(p, line.find('"', p) - p);
}

// Feeds a synthetic walk: 30 Hz poses moving 1.5 cm per frame, 200 Hz gyro, 10 Hz frames.
struct Feeder {
  ck_session* s;
  fs::path dir;
  int w = 320, h = 240;
  uint32_t frame_id = 0;
  bool ack = true;
  std::set<std::string> written;

  std::vector<std::string> seen;  // every outbound event line, in order
  void step(int64_t i, bool frames = true, int32_t tracking = 1, int32_t reason = 0) {
    int64_t t = (i + 1) * 33 * kMs;  // t_ns <= 0 is rejected at ingest
    ck_pose p{t, {0.015 * double(i), 0, 0}, {1, 0, 0, 0}, tracking, reason};
    EXPECT_EQ(ck_ingest_pose(s, &p, CK_POSE_OPENCV), CK_OK);
    for (int k = 0; k < 6; ++k) {
      ck_imu g{t + k * 5 * kMs, {0.01, 0, 0}};
      ck_ingest_gyro(s, &g);
    }
    if (frames && i % 3 == 0) {
      auto px = texture(w, h, uint32_t(i));
      ck_frame f{t + 1, frame_id++, 8 * kMs, px.data(), w, h, w};
      ck_ingest_frame(s, &f);
    }
  }
  void handle_events() {
    for (auto& e : drain(s)) {
      seen.push_back(e);
      if (e.find("\"keyframe_request\"") == std::string::npos) continue;
      auto file = json_str(e, "file");
      if (ack) {
        write_jpeg(dir / file);
        written.insert(file);
        EXPECT_EQ(ck_keyframe_written(s, json_u32(e, "frame_id"), 1), CK_OK);
      }
    }
  }
};

ck_session* make(const fs::path& d, const char* cfg) {
  ck_status err;
  auto* s = ck_session_create(d.c_str(), cfg, &err);
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(err, CK_OK);
  return s;
}

void intr(ck_session* s, int64_t t = 1) {
  ck_intrinsics k{t, 320, 240, 250, 250, 159.5, 119.5};
  EXPECT_EQ(ck_ingest_intrinsics(s, &k), CK_OK);
}
}  // namespace

TEST(SessionStateMachine, TransitionTable) {
  using session::State;
  using session::transition_allowed;
  EXPECT_TRUE(transition_allowed(State::Idle, State::Calibrating));
  EXPECT_TRUE(transition_allowed(State::Calibrating, State::Scanning));
  EXPECT_TRUE(transition_allowed(State::Scanning, State::Paused));
  EXPECT_TRUE(transition_allowed(State::Paused, State::Calibrating));
  EXPECT_TRUE(transition_allowed(State::Scanning, State::Finalizing));
  EXPECT_TRUE(transition_allowed(State::Finalizing, State::Complete));
  EXPECT_FALSE(transition_allowed(State::Idle, State::Scanning));
  EXPECT_FALSE(transition_allowed(State::Idle, State::Finalizing));
  EXPECT_FALSE(transition_allowed(State::Complete, State::Calibrating));
  EXPECT_FALSE(transition_allowed(State::Finalizing, State::Paused));
  EXPECT_FALSE(transition_allowed(State::Complete, State::Failed));
}

TEST(SessionStateMachine, IllegalApiCalls) {
  auto d = fresh("illegal");
  auto* s = make(d, "{\"threaded\":0}");
  ck_pose p{0, {0, 0, 0}, {1, 0, 0, 0}, 1, 0};
  EXPECT_EQ(ck_ingest_pose(s, &p, CK_POSE_OPENCV), CK_ERR_STATE);  // before start
  EXPECT_EQ(ck_session_pause(s, 0), CK_ERR_STATE);
  EXPECT_EQ(ck_session_resume(s, 0), CK_ERR_STATE);
  EXPECT_EQ(ck_session_finalize(s), CK_ERR_STATE);
  EXPECT_EQ(ck_session_start(s, 0), CK_OK);
  EXPECT_EQ(ck_session_start(s, 1), CK_ERR_STATE);
  EXPECT_EQ(ck_session_resume(s, 1), CK_ERR_STATE);  // not paused
  EXPECT_EQ(ck_session_pause(s, 2), CK_OK);
  EXPECT_EQ(ck_session_state(s), CK_STATE_PAUSED);
  EXPECT_EQ(ck_session_resume(s, 3), CK_OK);
  EXPECT_EQ(ck_session_state(s), CK_STATE_CALIBRATING);
  ck_intrinsics bad{0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(ck_ingest_intrinsics(s, &bad), CK_ERR_ARG);
  EXPECT_EQ(ck_ingest_pose(s, &p, 7), CK_ERR_ARG);
  EXPECT_EQ(ck_session_pump(s), CK_OK);
  ck_session_destroy(s);
  // A directory that already holds a session is refused (protects data).
  ck_status err;
  EXPECT_EQ(ck_session_create(d.c_str(), "{}", &err), nullptr);
  EXPECT_EQ(err, CK_ERR_IO);
  EXPECT_EQ(ck_session_create(fresh("badcfg").c_str(), "{\"nope\":1}", &err), nullptr);
  EXPECT_EQ(err, CK_ERR_ARG);
}

// Device bug (2026-11-17): the first /pose and /intrinsics reached the bundle at log_time 0 (unmapped clock).
TEST(SessionIngest, NonPositiveTimestampsAreRejectedAndCounted) {
  auto d = fresh("time_zero");
  auto* s = make(d, "{\"threaded\":0}");
  ASSERT_EQ(ck_session_start(s, 1), CK_OK);
  ck_pose p0{0, {0, 0, 0}, {1, 0, 0, 0}, 1, 0}, pn{-5, {0, 0, 0}, {1, 0, 0, 0}, 1, 0};
  ck_intrinsics k0{0, 320, 240, 250, 250, 159.5, 119.5};
  auto px = texture(64, 48, 1);
  ck_frame f0{0, 1, 0, px.data(), 64, 48, 64};
  EXPECT_EQ(ck_ingest_pose(s, &p0, CK_POSE_OPENCV), CK_ERR_ARG);
  EXPECT_EQ(ck_ingest_pose(s, &pn, CK_POSE_OPENCV), CK_ERR_ARG);
  EXPECT_EQ(ck_ingest_intrinsics(s, &k0), CK_ERR_ARG);
  EXPECT_EQ(ck_ingest_frame(s, &f0), CK_ERR_ARG);
  intr(s, 1);
  ck_pose ok{1, {0, 0, 0}, {1, 0, 0, 0}, 1, 0};
  EXPECT_EQ(ck_ingest_pose(s, &ok, CK_POSE_OPENCV), CK_OK);
  ck_frame f1{2, 2, 0, px.data(), 64, 48, 64};
  EXPECT_EQ(ck_ingest_frame(s, &f1), CK_OK);
  ck_session_pump(s);
  bool logged = false;
  for (auto& e : drain(s)) logged |= e.find("\"ingest_drop\"") != std::string::npos;
  EXPECT_TRUE(logged);
  ck_session_stop(s, 10);
  ck_session_pump(s);
  ASSERT_EQ(ck_session_finalize(s), CK_OK);
  ck_session_destroy(s);
  auto c = bundle::read_streams((d / "streams.mcap").string());
  ASSERT_EQ(c.poses.size(), 1u);
  EXPECT_EQ(c.poses[0].t_ns, 1);
  ASSERT_EQ(c.intrinsics.size(), 1u);
  EXPECT_EQ(c.intrinsics[0].t_ns, 1);
  bool drop_event = false;
  for (auto& e : c.events) drop_event |= e.code == "INGEST_DROP";
  EXPECT_TRUE(drop_event);
}

TEST(SessionEndToEnd, SyncScanFinalizesValidBundle) {
  auto d = fresh("e2e");
  auto* s = make(d, "{\"threaded\":0, \"platform\":\"test\"}");
  Feeder f{s, d};
  ASSERT_EQ(ck_session_start(s, 0), CK_OK);
  intr(s);
  for (int64_t i = 0; i < 300; ++i) {
    f.step(i);
    ck_session_pump(s);
    f.handle_events();
  }
  EXPECT_EQ(ck_session_state(s), CK_STATE_SCANNING);
  ASSERT_EQ(ck_session_stop(s, 300 * 33 * kMs), CK_OK);
  ck_session_pump(s);
  f.handle_events();  // final keyframe request from the open window
  ck_session_pump(s);
  ASSERT_EQ(ck_session_finalize(s), CK_OK);
  EXPECT_EQ(ck_session_state(s), CK_STATE_COMPLETE);
  ck_session_destroy(s);

  EXPECT_TRUE(fs::exists(d / "manifest.json"));
  EXPECT_TRUE(fs::exists(d / "camera.json"));
  EXPECT_FALSE(fs::exists(d / "session.json"));
  auto c = bundle::read_streams((d / "streams.mcap").string());
  EXPECT_TRUE(c.finalized);
  EXPECT_EQ(c.poses.size(), 300u);
  ASSERT_GT(c.frames.size(), 50u);
  int64_t prev = -1;
  size_t kf = 0;
  for (auto& fr : c.frames) {
    EXPECT_GT(fr.t_ns, prev);  // reorder buffer keeps /frame_index strictly increasing
    prev = fr.t_ns;
    if (fr.keyframe) {
      ++kf;
      EXPECT_TRUE(fs::exists(d / fr.file)) << fr.file;
    }
    EXPECT_GT(fr.sharpness, 0);
  }
  EXPECT_GT(kf, 5u);
  EXPECT_EQ(kf, f.written.size());
  std::set<std::string> codes;
  for (auto& e : c.events) codes.insert(e.code);
  for (auto* code : {"SESSION_STARTED", "QUALITY_CONFIG", "SESSION_STOPPED", "SESSION_FINALIZED"})
    EXPECT_TRUE(codes.count(code)) << code;
}

TEST(SessionQueues, OverflowIsDroppedCountedAndLogged) {
  auto d = fresh("overflow");
  auto* s = make(d, "{\"threaded\":0, \"queue_capacity\":16, \"frame_slots\":2}");
  ck_session_start(s, 0);
  int full = 0;
  for (int i = 0; i < 100; ++i) {
    ck_imu g{i * 5 * kMs, {0, 0, 0}};
    full += ck_ingest_gyro(s, &g) == CK_ERR_FULL;
  }
  EXPECT_EQ(full, 100 - 16 + 1);  // start's control item uses one queue entry
  ck_session_pump(s);
  auto px = texture(64, 48, 1);
  int slot_full = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    ck_frame fr{int64_t(1000 + i) * kMs, i, 0, px.data(), 64, 48, 64};
    slot_full += ck_ingest_frame(s, &fr) == CK_ERR_FULL;
  }
  EXPECT_EQ(slot_full, 3);
  ck_session_pump(s);
  bool logged = false;
  for (auto& e : drain(s)) logged |= e.find("\"ingest_drop\"") != std::string::npos;
  EXPECT_TRUE(logged);
  intr(s);
  ck_session_finalize(s);
  ck_session_destroy(s);
  auto c = bundle::read_streams((d / "streams.mcap").string());
  bool drop_event = false;
  for (auto& e : c.events) drop_event |= e.code == "INGEST_DROP";
  EXPECT_TRUE(drop_event);
}

TEST(SessionGovernor, CriticalThermalPausesAndBlocksResume) {
  auto d = fresh("governor");
  auto* s = make(d, "{\"threaded\":0}");
  Feeder f{s, d};
  ck_session_start(s, 0);
  intr(s);
  for (int64_t i = 0; i < 30; ++i) f.step(i, false);
  ck_session_pump(s);
  EXPECT_EQ(ck_session_state(s), CK_STATE_SCANNING);
  ck_platform hot{3, 0, 80, 0, 20};
  ck_ingest_platform(s, 31 * 33 * kMs, &hot);
  ck_session_pump(s);
  EXPECT_EQ(ck_session_state(s), CK_STATE_PAUSED);
  ck_policy pol;
  ck_policy_get(s, &pol);
  EXPECT_TRUE(pol.must_pause);
  EXPECT_EQ(ck_session_resume(s, 32 * 33 * kMs), CK_ERR_POLICY);
  bool gov = false;
  for (auto& e : drain(s)) gov |= e.find("\"governor\"") != std::string::npos;
  EXPECT_TRUE(gov);
  EXPECT_EQ(ck_session_finalize(s), CK_OK);
  ck_session_destroy(s);
}

TEST(SessionKeyframes, UnackedRequestsExpireAndLeaveNoOrphans) {
  auto d = fresh("expire");
  auto* s = make(d, "{\"threaded\":0, \"keyframe_ack_timeout_s\":0.5}");
  Feeder f{s, d};
  f.ack = false;
  ck_session_start(s, 0);
  intr(s);
  for (int64_t i = 0; i < 200; ++i) {
    f.step(i);
    ck_session_pump(s);
  }
  bool expired = false;
  for (auto& e : drain(s)) expired |= e.find("\"keyframe_expired\"") != std::string::npos;
  EXPECT_TRUE(expired);
  write_jpeg(d / "keyframes" / "000000.jpg");  // shell wrote it too late: must not be listed
  ASSERT_EQ(ck_session_finalize(s), CK_OK);
  ck_session_destroy(s);
  auto c = bundle::read_streams((d / "streams.mcap").string());
  size_t missing = 0;
  for (auto& fr : c.frames) {
    EXPECT_FALSE(fr.keyframe);
    for (auto& r : fr.reasons) missing += r == "ENCODE_MISSING";
  }
  EXPECT_GT(missing, 0u);
  EXPECT_FALSE(fs::exists(d / "keyframes" / "000000.jpg"));
}

TEST(SessionThreaded, ConcurrentIngestStress) {
  auto d = fresh("stress");
  auto* s = make(d, "{\"threaded\":1, \"queue_capacity\":512, \"record_imu\":1}");  // this test counts /imu/* records
  ck_session_start(s, 0);
  intr(s);
  const int N = 3000;
  std::atomic<int> ok[4] = {}, full[4] = {};
  auto count = [&](int k, ck_status st) {
    if (st == CK_OK) ++ok[k];
    else if (st == CK_ERR_FULL) ++full[k];
    else ADD_FAILURE() << "unexpected status " << st;
  };
  std::thread tp([&] {
    for (int i = 0; i < N; ++i) {
      ck_pose p{(i + 1) * 33 * kMs, {0.01 * i, 0, 0}, {1, 0, 0, 0}, 1, 0};
      count(0, ck_ingest_pose(s, &p, CK_POSE_OPENCV));
    }
  });
  std::thread tg([&] {
    for (int i = 0; i < N; ++i) {
      ck_imu g{i * 5 * kMs, {0.01, 0, 0}};
      count(1, ck_ingest_gyro(s, &g));
    }
  });
  std::thread ta([&] {
    for (int i = 0; i < N; ++i) {
      ck_imu a{i * 5 * kMs + 1, {0, 9.8, 0}};
      count(2, ck_ingest_accel(s, &a));
    }
  });
  std::thread tf([&] {
    auto px = texture(160, 120, 3);
    for (int i = 0; i < N / 10; ++i) {
      ck_frame fr{(i + 1) * 330 * kMs + 2, uint32_t(i), 0, px.data(), 160, 120, 160};
      count(3, ck_ingest_frame(s, &fr));
    }
  });
  std::thread tpoll([&] {
    char buf[4096];
    for (int i = 0; i < 2000; ++i) ck_poll_events(s, buf, sizeof buf);
  });
  for (auto* t : {&tp, &tg, &ta, &tf, &tpoll}) t->join();
  ASSERT_EQ(ck_session_stop(s, N * 33 * kMs), CK_OK);
  ASSERT_EQ(ck_session_finalize(s), CK_OK);
  ck_session_destroy(s);
  EXPECT_EQ(ok[0] + full[0], N);
  EXPECT_EQ(ok[1] + full[1], N);
  auto c = bundle::read_streams((d / "streams.mcap").string());
  EXPECT_EQ(c.poses.size(), size_t(ok[0].load()));
  EXPECT_EQ(c.gyro.size(), size_t(ok[1].load()));
  EXPECT_EQ(c.accel.size(), size_t(ok[2].load()));
  EXPECT_TRUE(c.finalized);
}

TEST(SessionRecovery, KilledSessionBecomesRecoveredBundle) {
  auto d = fresh("recover");
  auto* s = make(d, "{\"threaded\":0, \"model\":\"TestPhone\"}");
  Feeder f{s, d};
  ck_session_start(s, 0);
  intr(s);
  for (int64_t i = 0; i < 240; ++i) {
    f.step(i);
    ck_session_pump(s);
    f.handle_events();
  }
  ASSERT_GT(f.written.size(), 2u);
  // Simulate a crash: a half-written JPEG that was acked, a temp file, then no finalize.
  auto victim = *f.written.begin();
  write_jpeg(d / victim, /*complete=*/false);
  write_jpeg(d / "keyframes" / "999999.jpg.tmp");
  ck_session_destroy(s);
  ASSERT_FALSE(fs::exists(d / "manifest.json"));

  char rep[1024];
  ASSERT_EQ(ck_recover(d.c_str(), rep, sizeof rep), CK_OK) << rep;
  std::string r(rep);
  EXPECT_NE(r.find("\"ok\":true"), std::string::npos) << r;
  EXPECT_NE(r.find("\"dropped_frame_records\":1"), std::string::npos) << r;
  EXPECT_TRUE(fs::exists(d / "manifest.json"));
  EXPECT_FALSE(fs::exists(d / "session.json"));
  EXPECT_FALSE(fs::exists(d / "keyframes" / "999999.jpg.tmp"));
  EXPECT_FALSE(fs::exists(d / victim));
  std::ifstream m(d / "manifest.json");
  std::string manifest((std::istreambuf_iterator<char>(m)), std::istreambuf_iterator<char>());
  EXPECT_NE(manifest.find("recovered"), std::string::npos);
  EXPECT_NE(manifest.find("TestPhone"), std::string::npos);
  auto c = bundle::read_streams((d / "streams.mcap").string());
  EXPECT_TRUE(c.finalized);
  bool rec_event = false;
  for (auto& e : c.events) rec_event |= e.code == "SESSION_RECOVERED";
  EXPECT_TRUE(rec_event);
  // Idempotent.
  EXPECT_EQ(ck_recover(d.c_str(), rep, sizeof rep), CK_OK);
  EXPECT_NE(std::string(rep).find("\"already_complete\":true"), std::string::npos);
}

// ADR 0020: consent / privacy metadata (schema 1.2.0), shell-supplied, kept across a crash.
namespace {
std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
constexpr const char* kConsentCfg =
    "{\"threaded\":0, \"model\":\"ConsentPhone\", \"consent_version\":\"v1\", "
    "\"consent_at\":\"2026-11-17T09:30:00Z\", \"redaction_mode\":\"faces\", \"record_imu\":1}";  // 1.2.0 = IMU recorded
void expect_consent_manifest(const std::string& m) {
  EXPECT_NE(m.find("\"schema_version\": \"1.2.0\""), std::string::npos) << m.substr(0, 200);
  EXPECT_NE(m.find("\"consent\": {\"version\": \"v1\", \"at\": \"2026-11-17T09:30:00Z\"}"), std::string::npos);
  EXPECT_NE(m.find("\"privacy\": {\"redaction_mode\": \"faces\"}"), std::string::npos);
}
}  // namespace

TEST(SessionConsent, BadConfigIsRejected) {
  for (const char* cfg : {"{\"consent_version\":\"v1\"}", "{\"consent_at\":\"2026-11-17T09:30:00Z\"}",
                          "{\"redaction_mode\":\"blur\"}", "{\"consent_version\":5, \"consent_at\":\"x\"}"}) {
    ck_status err = CK_OK;
    auto d = fresh("consent_bad");
    auto* s = ck_session_create(d.c_str(), cfg, &err);
    EXPECT_EQ(s, nullptr) << cfg;
    EXPECT_NE(err, CK_OK) << cfg;
    if (s) ck_session_destroy(s);
  }
}

TEST(SessionConsent, FinalizedBundleAndRecoveredBundleKeepConsent) {
  auto run = [](const char* name, bool finalize) {
    auto d = fresh(name);
    auto* s = make(d, kConsentCfg);
    Feeder f{s, d};
    EXPECT_EQ(ck_session_start(s, 0), CK_OK);
    intr(s);
    for (int64_t i = 0; i < 120; ++i) {
      f.step(i);
      ck_session_pump(s);
      f.handle_events();
    }
    if (finalize) {
      EXPECT_EQ(ck_session_stop(s, 120 * 33 * kMs), CK_OK);
      ck_session_pump(s);
      f.handle_events();
      ck_session_pump(s);
      EXPECT_EQ(ck_session_finalize(s), CK_OK);
    }
    ck_session_destroy(s);
    return d;
  };
  auto done = run("consent_final", true);
  expect_consent_manifest(slurp(done / "manifest.json"));

  auto killed = run("consent_killed", false);
  ASSERT_FALSE(fs::exists(killed / "manifest.json"));
  EXPECT_NE(slurp(killed / "session.json").find("\"consent_version\":\"v1\""), std::string::npos);
  char rep[1024];
  ASSERT_EQ(ck_recover(killed.c_str(), rep, sizeof rep), CK_OK) << rep;
  auto m = slurp(killed / "manifest.json");
  expect_consent_manifest(m);
  EXPECT_NE(m.find("recovered"), std::string::npos);
}

// Device bug (2026-11-17): killed before the first redaction.json flush -> recovered bundle was REDACTION_MANIFEST_MISSING.
TEST(SessionRecovery, FacesModeWithoutRedactionLogGetsEmptyMarkedLog) {
  auto d = fresh("recover_faces");
  auto* s = make(d, kConsentCfg);
  Feeder f{s, d};
  ck_session_start(s, 1);
  intr(s);
  for (int64_t i = 0; i < 120; ++i) {
    f.step(i);
    ck_session_pump(s);
    f.handle_events();
  }
  ck_session_destroy(s);  // killed: no finalize
  ASSERT_FALSE(fs::exists(d / "privacy" / "redaction.json"));
  fs::create_directories(d / "privacy");
  { std::ofstream(d / "privacy" / "redaction.json.tmp") << "{half"; }  // interrupted flush
  char rep[1024];
  ASSERT_EQ(ck_recover(d.c_str(), rep, sizeof rep), CK_OK) << rep;
  auto log = slurp(d / "privacy" / "redaction.json");
  EXPECT_NE(log.find("\"keyframes\":[]"), std::string::npos) << log;
  EXPECT_NE(log.find("\"recovered\":true"), std::string::npos) << log;
  EXPECT_FALSE(fs::exists(d / "privacy" / "redaction.json.tmp"));
  EXPECT_FALSE(fs::exists(d / "privacy" / "redaction.json.recovering"));
  auto m = slurp(d / "manifest.json");
  EXPECT_NE(m.find("privacy/redaction.json"), std::string::npos);
  EXPECT_NE(m.find("\"role\": \"redaction\""), std::string::npos) << m;
}

// An existing log (written by the shell) is never overwritten by recovery.
TEST(SessionRecovery, ExistingRedactionLogIsKept) {
  auto d = fresh("recover_faces_keep");
  auto* s = make(d, kConsentCfg);
  Feeder f{s, d};
  ck_session_start(s, 1);
  intr(s);
  for (int64_t i = 0; i < 60; ++i) {
    f.step(i);
    ck_session_pump(s);
    f.handle_events();
  }
  ck_session_destroy(s);
  fs::create_directories(d / "privacy");
  const std::string mine = "{\"version\":1,\"mode\":\"faces\",\"keyframes\":[]}";
  { std::ofstream(d / "privacy" / "redaction.json") << mine; }
  char rep[1024];
  ASSERT_EQ(ck_recover(d.c_str(), rep, sizeof rep), CK_OK) << rep;
  EXPECT_EQ(slurp(d / "privacy" / "redaction.json"), mine);
}

// U2-CT left /ar/detections out of recover(): a recovered bundle must keep it (schema 1.1.0, ADR 0016).
TEST(SessionRecovery, RecoveredBundleCarriesDetections) {
  auto d = fresh("recover_det");
  fs::create_directories(d);
  {
    bundle::StreamWriter w;
    ASSERT_TRUE(w.open((d / "streams.mcap").string()));
    bundle::IntrinsicsSample k;
    k.t_ns = 0; k.width = 320; k.height = 240; k.fx = k.fy = 250; k.cx = 159.5; k.cy = 119.5;
    w.write(k);
    for (int64_t i = 0; i < 20; ++i) {
      bundle::PoseSample p;
      p.t_ns = i * 33 * kMs;
      w.write(p);
    }
    for (uint32_t id = 0; id < 3; ++id) {
      bundle::FrameRecord fr;
      fr.t_ns = int64_t(id) * 100 * kMs + 1;
      fr.frame_id = id;
      w.write(fr);
      bundle::DetectionsRecord dr;
      dr.t_ns = fr.t_ns;
      dr.frame_id = id;
      if (id != 1) dr.boxes.push_back({3, 0.75f, 10, 20, 110, 220});  // id 1: "ran, saw nothing"
      w.write(dr);
    }
    w.close();
  }
  {
    std::ofstream f(d / "session.json");
    f << "{\"bundle_id\":\"b\",\"session_id\":\"s\",\"consent_version\":\"v1\",\"consent_at\":\"2026-11-17T09:30:00Z\","
         "\"redaction_mode\":\"faces\"}\n";
  }
  char rep[1024];
  ASSERT_EQ(ck_recover(d.c_str(), rep, sizeof rep), CK_OK) << rep;
  auto c = bundle::read_streams((d / "streams.mcap").string());
  EXPECT_TRUE(c.finalized);
  ASSERT_EQ(c.detections.size(), 3u);
  EXPECT_EQ(c.detections[0].boxes.size(), 1u);
  EXPECT_EQ(c.detections[0].boxes[0].cls, 3);
  EXPECT_FLOAT_EQ(c.detections[0].boxes[0].x1, 110.f);
  EXPECT_TRUE(c.detections[1].boxes.empty());
  auto m = slurp(d / "manifest.json");
  EXPECT_NE(m.find("\"detections\": 3"), std::string::npos) << m;
  expect_consent_manifest(m);
}

TEST(SessionDetections, ReachBundleAreBoundedAndDropsAreCounted) {
  auto d = fresh("detections");
  auto* s = make(d, "{\"threaded\":0, \"queue_capacity\":16, \"frame_slots\":2}");
  ck_detection bx[2] = {{3, 0.75f, 10, 20, 110, 220}, {1, 0.5f, 0, 0, 50, 60}};
  EXPECT_EQ(ck_ingest_detections(s, 1, 0, bx, 2), CK_ERR_STATE);  // not capturing yet
  ASSERT_EQ(ck_session_start(s, 0), CK_OK);
  ck_session_pump(s);
  intr(s);
  ck_session_pump(s);
  EXPECT_EQ(ck_ingest_detections(nullptr, 1, 0, bx, 2), CK_ERR_ARG);
  EXPECT_EQ(ck_ingest_detections(s, 1, 0, nullptr, 2), CK_ERR_ARG);
  EXPECT_EQ(ck_ingest_detections(s, 1, 0, bx, 100000), CK_ERR_ARG);  // absurd box count
  EXPECT_EQ(ck_ingest_detections(s, 100 * kMs, 7, bx, 2), CK_OK);
  EXPECT_EQ(ck_ingest_detections(s, 200 * kMs, 8, nullptr, 0), CK_OK);  // ran, saw nothing
  ck_session_pump(s);
  int full = 0;  // the queue holds 16: the rest are refused and counted
  for (uint32_t i = 0; i < 40; ++i) full += ck_ingest_detections(s, (300 + i) * kMs, 20 + i, bx, 1) == CK_ERR_FULL;
  EXPECT_EQ(full, 40 - 16);
  ck_session_pump(s);
  ASSERT_EQ(ck_session_finalize(s), CK_OK);
  bool logged = false;
  for (auto& e : drain(s))
    logged |= e.find("\"ingest_drop\"") != std::string::npos && e.find("\"detections\":24") != std::string::npos;
  EXPECT_TRUE(logged);
  ck_session_destroy(s);
  auto c = bundle::read_streams((d / "streams.mcap").string());
  ASSERT_EQ(c.detections.size(), 2u + 16u);
  EXPECT_EQ(c.detections[0].frame_id, 7u);
  ASSERT_EQ(c.detections[0].boxes.size(), 2u);
  EXPECT_EQ(c.detections[0].boxes[0].cls, 3);
  EXPECT_FLOAT_EQ(c.detections[0].boxes[0].y1, 220.f);
  EXPECT_TRUE(c.detections[1].boxes.empty());
  bool drop_event = false;
  for (auto& e : c.events) drop_event |= e.code == "INGEST_DROP";
  EXPECT_TRUE(drop_event);
  auto m = slurp(d / "manifest.json");
  EXPECT_NE(m.find("\"detections\": 18"), std::string::npos) << m;
}

TEST(SessionRecovery, TruncatedStreamsNeverCrash) {
  auto src = fresh("trunc_src");
  auto* s = make(src, "{\"threaded\":0}");
  Feeder f{s, src};
  ck_session_start(s, 0);
  intr(s);
  for (int64_t i = 0; i < 150; ++i) {
    f.step(i);
    ck_session_pump(s);
    f.handle_events();
  }
  ck_session_destroy(s);
  const auto size = fs::file_size(src / "streams.mcap");
  int recovered = 0;
  for (double frac : {0.02, 0.1, 0.33, 0.5, 0.77, 0.99}) {
    auto d = fresh("trunc_" + std::to_string(int(frac * 100)));
    fs::copy(src, d, fs::copy_options::recursive);
    fs::resize_file(d / "streams.mcap", uintmax_t(double(size) * frac));
    char rep[1024];
    auto st = ck_recover(d.c_str(), rep, sizeof rep);
    if (st == CK_OK) {
      ++recovered;
      EXPECT_TRUE(bundle::read_streams((d / "streams.mcap").string()).finalized);
    } else {
      EXPECT_NE(std::string(rep).find("\"error\""), std::string::npos);
    }
  }
  EXPECT_GE(recovered, 4);
}

TEST(SessionValidation, ManifestCarriesOnDeviceVerdict) {
  auto d = fresh("verdict");
  auto* s = make(d, "{\"threaded\":0}");
  Feeder f{s, d};
  ck_session_start(s, 0);
  intr(s);
  for (int64_t i = 0; i < 200; ++i) {
    f.step(i);
    ck_session_pump(s);
    f.handle_events();
  }
  ASSERT_EQ(ck_session_finalize(s), CK_OK);
  ck_session_destroy(s);
  std::ifstream m(d / "manifest.json");
  std::string manifest((std::istreambuf_iterator<char>(m)), std::istreambuf_iterator<char>());
  EXPECT_NE(manifest.find("\"validation\""), std::string::npos);
  EXPECT_NE(manifest.find("\"verdict\":\"PASS\""), std::string::npos) << manifest;  // INFO-only is a PASS
  EXPECT_NE(manifest.find("NO_AR_GEOMETRY"), std::string::npos);
}

TEST(SessionValidation, SemanticChecksFireOnBrokenInput) {
  using namespace capturekit::bundle;
  StreamContents s;
  PoseSample p;
  p.t_ns = 1000;
  p.q_world_from_camera_wxyz[0] = 2;  // non-unit
  s.poses.push_back(p);
  p.t_ns = 1000 + 2'000'000'000;  // 2 s gap
  p.q_world_from_camera_wxyz[0] = 1;
  p.t_world_from_camera[0] = std::nan("");
  s.poses.push_back(p);
  FrameRecord fr;
  fr.t_ns = 500;  // before any pose and before intrinsics
  fr.keyframe = true;
  fr.file = "keyframes/none.jpg";
  fr.tracking_state = TrackingState::Limited;
  s.frames.push_back(fr);
  auto r = validate_semantic(CK_TEST_TMP, s, "recovered", nullptr);
  for (auto code : {"NAN_VALUE", "MISSING_INTRINSICS", "KEYFRAME_WITHOUT_POSE", "KEYFRAME_FILE_MISSING",
                    "KEYFRAME_TRACKING_LIMITED", "POSE_GAP", "FEW_KEYFRAMES", "NO_AR_GEOMETRY", "PARTIAL_BUNDLE"})
    EXPECT_TRUE(r.has(code)) << code;
  EXPECT_EQ(r.verdict(), "FAIL");
}

// Android brief B1: the guidance core emits a non-critical "overlay" event every 500 ms of EVENT time.
namespace {
std::vector<std::string> overlay_events(const char* name) {
  auto d = fresh(name);
  auto* s = make(d, "{\"threaded\":0, \"platform\":\"test\"}");
  Feeder f{s, d};
  EXPECT_EQ(ck_session_start(s, 0), CK_OK);
  intr(s);
  std::vector<std::string> out;
  for (int64_t i = 0; i < 300; ++i) {
    f.step(i);
    ck_session_pump(s);
    for (auto& e : drain(s)) {
      if (e.find("\"keyframe_request\"") != std::string::npos) {
        auto file = json_str(e, "file");
        write_jpeg(d / file);
        EXPECT_EQ(ck_keyframe_written(s, json_u32(e, "frame_id"), 1), CK_OK);
      } else if (e.find("\"type\":\"overlay\"") != std::string::npos) {
        out.push_back(e);
      }
    }
  }
  ck_session_stop(s, 300 * 33 * kMs);
  ck_session_pump(s);
  ck_session_finalize(s);
  ck_session_destroy(s);
  return out;
}
}  // namespace

TEST(SessionOverlay, EmittedEvery500msOfEventTimeSmallAndDeterministic) {
  auto a = overlay_events("overlay_a");
  ASSERT_GE(a.size(), 15u);  // 9.9 s of event time
  ASSERT_LE(a.size(), 21u);
  int64_t prev = -1;
  for (auto& e : a) {
    EXPECT_LT(e.size(), 4096u);
    auto p = e.find("\"t_ns\":");
    int64_t t = std::stoll(e.substr(p + 7));
    if (prev >= 0) {
      EXPECT_GE(t - prev, 500 * kMs);
      EXPECT_LT(t - prev, 500 * kMs + 34 * kMs);  // next pose after the 500 ms mark
    }
    prev = t;
  }
  EXPECT_EQ(a, overlay_events("overlay_b"));  // same input stream -> byte-identical events
}

// Phase 9 step 5: the session forwards recorded detections to the engine; objects appear in the overlay only with
// guide.understanding on.
namespace {
std::vector<std::string> overlays_with_detections(const char* name, const char* cfg) {
  auto d = fresh(name);
  auto* s = make(d, cfg);
  EXPECT_EQ(ck_session_start(s, 0), CK_OK);
  intr(s);
  ck_plane fl{0, "f", "floor", {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, 6, 6};
  ck_plane wl{0, "w1", "wall", {0, 1.25, 4}, {0, 0, -1}, {1, 0, 0}, 5, 2.5};
  EXPECT_EQ(ck_ingest_plane(s, &fl), CK_OK);
  EXPECT_EQ(ck_ingest_plane(s, &wl), CK_OK);
  // A chair (0.5 x 0.9 x 0.5 m) 3.5 m ahead of a camera 1.4 m up (pinhole f = 250, 320 x 240; camera upright in an
  // OpenCV frame = rotation of 180 degrees about the optical axis relative to world axes).
  ck_detection chair{1, 0.8f, 159.5f - 17.9f, 155.2f, 159.5f + 17.9f, 219.5f};
  std::vector<std::string> out;
  for (int64_t i = 0; i < 150; ++i) {
    ck_pose p{(i + 1) * 33 * kMs, {0, 1.4, 0}, {0, 0, 0, 1}, 1, 0};
    EXPECT_EQ(ck_ingest_pose(s, &p, CK_POSE_OPENCV), CK_OK);
    if (i == 30 || i == 36 || i == 42) EXPECT_EQ(ck_ingest_detections(s, (i + 1) * 33 * kMs, uint32_t(i), &chair, 1), CK_OK);
    ck_session_pump(s);
    for (auto& e : drain(s))
      if (e.find("\"type\":\"overlay\"") != std::string::npos) out.push_back(e);
  }
  ck_session_stop(s, 150 * 33 * kMs);
  ck_session_pump(s);
  ck_session_finalize(s);
  ck_session_destroy(s);
  return out;
}
}  // namespace

TEST(SessionDetections, ForwardedToEngineOnlyWithUnderstanding) {
  auto on = overlays_with_detections("det_engine_on", "{\"threaded\":0, \"platform\":\"test\", \"guide.understanding\":1}");
  ASSERT_FALSE(on.empty());
  EXPECT_EQ(on.front().find("\"objects\""), std::string::npos);  // before the detections
  EXPECT_NE(on.back().find("\"objects\":\"1,chair,"), std::string::npos) << on.back();
  for (auto& e : on) EXPECT_LT(e.size(), 4096u);
  auto off = overlays_with_detections("det_engine_off", "{\"threaded\":0, \"platform\":\"test\"}");
  ASSERT_FALSE(off.empty());
  for (auto& e : off) EXPECT_EQ(e.find("\"objects\""), std::string::npos);
  EXPECT_EQ(on, overlays_with_detections("det_engine_on2", "{\"threaded\":0, \"platform\":\"test\", \"guide.understanding\":1}"));
}

// ADR 0028: with understanding on, the engine's published labels reach /guide/* (change-triggered + final) and the
// manifest is 1.3.0; with it off nothing is written and the manifest stays 1.1.0. Deterministic.
TEST(SessionGuideOutputs, WrittenOnlyWithUnderstandingAndDeterministic) {
  // record_imu:1 keeps these the pre-1.4.0 shapes (1.3.0 / 1.1.0); the lean default has its own test below.
  const char* on_cfg = "{\"threaded\":0, \"platform\":\"test\", \"guide.understanding\":1, \"record_imu\":1}";
  auto dir = [](const char* n) { return fs::path(CK_TEST_TMP) / "session" / n; };
  auto slurp = [](const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  overlays_with_detections("guide_out_on", on_cfg);
  auto c = bundle::read_streams((dir("guide_out_on") / "streams.mcap").string());
  ASSERT_TRUE(c.finalized);
  ASSERT_FALSE(c.labels.empty());
  for (size_t i = 1; i < c.labels.size(); ++i) EXPECT_GT(c.labels[i].t_ns, c.labels[i - 1].t_ns);
  ASSERT_EQ(c.labels.back().objects.size(), 1u);
  EXPECT_EQ(c.labels.back().objects[0].cls, 1);  // chair
  EXPECT_GE(c.labels.back().objects[0].n_obs, 2u);
  auto m = slurp(dir("guide_out_on") / "manifest.json");
  EXPECT_NE(m.find("\"schema_version\": \"1.3.0\""), std::string::npos) << m.substr(0, 120);
  EXPECT_NE(m.find("\"guide_labels\": " + std::to_string(c.labels.size())), std::string::npos);

  overlays_with_detections("guide_out_off", "{\"threaded\":0, \"platform\":\"test\", \"record_imu\":1}");
  auto off = bundle::read_streams((dir("guide_out_off") / "streams.mcap").string());
  EXPECT_TRUE(off.labels.empty() && off.shell.empty() && off.gaps.empty());
  auto mo = slurp(dir("guide_out_off") / "manifest.json");
  EXPECT_NE(mo.find("\"schema_version\": \"1.1.0\""), std::string::npos);
  EXPECT_EQ(mo.find("guide_labels"), std::string::npos);

  overlays_with_detections("guide_out_on2", on_cfg);
  auto c2 = bundle::read_streams((dir("guide_out_on2") / "streams.mcap").string());
  ASSERT_EQ(c2.labels.size(), c.labels.size());
  ASSERT_EQ(c2.shell.size(), c.shell.size());
  ASSERT_EQ(c2.gaps.size(), c.gaps.size());
  for (size_t i = 0; i < c.labels.size(); ++i) {
    EXPECT_EQ(c2.labels[i].t_ns, c.labels[i].t_ns);
    EXPECT_EQ(bundle::codec::encode(c2.labels[i]), bundle::codec::encode(c.labels[i]));
  }
}

// Issue #1 / ADR 0031: covering the camera (ARCore PAUSED + INSUFFICIENT_FEATURES) for 3 s mid-scan. Recovery starts after
// 2 s: the shell gets tracking_lost + a RECALIBRATE prompt and nothing else, keyframe capture pauses (frames recorded as
// RECALIBRATING hard rejects), and once tracking is back and steady the shell gets tracking_reset, scanning resumes and
// the bundle's /events timeline has TRACKING_LOST and TRACKING_RESET (with the drift delta).
namespace {
int64_t json_i64(const std::string& line, const std::string& key) {
  auto p = line.find("\"" + key + "\":");
  return std::stoll(line.substr(p + key.size() + 3));
}
}  // namespace

TEST(SessionTrackingRecovery, KeyframesPausedPromptOnlyAndTimelineEvents) {
  auto d = fresh("tracking_recovery");
  auto* s = make(d, "{\"threaded\":0, \"platform\":\"test\"}");
  Feeder f{s, d};
  ASSERT_EQ(ck_session_start(s, 0), CK_OK);
  intr(s);
  auto feed = [&](int64_t from, int64_t to, int32_t tracking, int32_t reason) {
    for (int64_t i = from; i < to; ++i) {
      f.step(i, true, tracking, reason);
      ck_session_pump(s);
      f.handle_events();
    }
  };
  feed(0, 150, 1, 0);    // 5 s scanning
  feed(150, 240, 2, 3);  // 3 s camera covered: LIMITED / INSUFFICIENT_FEATURES
  feed(240, 420, 1, 0);  // 6 s scanning again
  ASSERT_EQ(ck_session_stop(s, 420 * 33 * kMs), CK_OK);
  ck_session_pump(s);
  f.handle_events();
  ck_session_pump(s);
  ASSERT_EQ(ck_session_finalize(s), CK_OK);
  ck_session_destroy(s);

  // Outbound (shell) events.
  int64_t lost_t = -1, reset_t = -1;
  int n_lost = 0, n_reset = 0, n_recal = 0, n_flagged = 0;
  for (auto& e : f.seen) {
    if (e.find("\"type\":\"tracking_lost\"") != std::string::npos) {
      ++n_lost, lost_t = json_i64(e, "t_ns");
      EXPECT_EQ(json_i64(e, "tracking_state"), 2);
      EXPECT_EQ(json_i64(e, "reason"), 3);
    } else if (e.find("\"type\":\"tracking_reset\"") != std::string::npos) {
      ++n_reset, reset_t = json_i64(e, "t_ns");
      EXPECT_NE(e.find("\"drift_m\":"), std::string::npos);
      EXPECT_NE(e.find("\"lost_s\":"), std::string::npos);
    }
  }
  ASSERT_EQ(n_lost, 1);
  ASSERT_EQ(n_reset, 1);
  EXPECT_EQ(lost_t, 212 * 33 * kMs);  // pose 211 (t = 212 * 33 ms) is the first >= 2 s after pose 150 (61 x 33 ms)
  EXPECT_GT(reset_t, 240 * 33 * kMs + 990 * kMs);  // settled for 1 s (the feeder's gyro is steady)
  EXPECT_LT(reset_t, 240 * 33 * kMs + 1100 * kMs);
  for (auto& e : f.seen) {
    if (e.find("\"type\":\"instruction\"") != std::string::npos) {
      const int64_t t = json_i64(e, "t_ns");
      if (t >= lost_t && t < reset_t) {
        EXPECT_NE(e.find("\"instruction\":\"RECALIBRATE\""), std::string::npos) << e;  // nothing else while recovering
        EXPECT_NE(e.find("\"overlay_hint\":\"recalibrate\""), std::string::npos) << e;
        ++n_recal;
      }
    }
    if (e.find("\"type\":\"frame_quality\"") != std::string::npos && e.find("RECALIBRATING") != std::string::npos) {
      const int64_t t = json_i64(e, "t_ns");
      EXPECT_GT(t, lost_t);
      EXPECT_LT(t, reset_t);
      EXPECT_NE(e.find("\"hard_reject\":true"), std::string::npos);
      ++n_flagged;
    }
  }
  EXPECT_EQ(n_recal, 1);
  EXPECT_GE(n_flagged, 8);  // ~10 candidate frames per second of recovery

  // Bundle: no keyframe while recovering, keyframes again afterwards, both events on the /events timeline.
  auto c = bundle::read_streams((d / "streams.mcap").string());
  ASSERT_TRUE(c.finalized);
  size_t kf_after = 0, recal_frames = 0;
  for (auto& fr : c.frames) {
    const bool recal = std::find(fr.reasons.begin(), fr.reasons.end(), "RECALIBRATING") != fr.reasons.end();
    if (fr.t_ns > lost_t && fr.t_ns < reset_t) {
      EXPECT_FALSE(fr.keyframe) << fr.t_ns;
      EXPECT_TRUE(recal) << fr.t_ns;
      ++recal_frames;
    } else {
      EXPECT_FALSE(recal) << fr.t_ns;
    }
    if (fr.keyframe && fr.t_ns > reset_t) ++kf_after;
  }
  EXPECT_GE(recal_frames, 8u);
  EXPECT_GT(kf_after, 0u);  // scanning continued seamlessly
  const bundle::EventRecord* lost = nullptr;
  const bundle::EventRecord* reset = nullptr;
  for (auto& e : c.events) {
    if (e.code == "TRACKING_LOST") lost = &e;
    if (e.code == "TRACKING_RESET") reset = &e;
  }
  ASSERT_NE(lost, nullptr);
  ASSERT_NE(reset, nullptr);
  EXPECT_EQ(lost->source, "guide");
  EXPECT_EQ(lost->t_ns, lost_t);
  EXPECT_EQ(reset->t_ns, reset_t);
  std::set<std::string> keys;
  for (auto& [k, v] : reset->attrs) keys.insert(k);
  EXPECT_EQ(keys, (std::set<std::string>{"drift_deg", "drift_m", "lost_s"}));
  EXPECT_TRUE(fs::exists(d / "manifest.json"));
}

// ADR 0034 (issue #10, lean bundle): the live session leaves the IMU out of the bundle unless record_imu is set. The
// manifest says so (schema 1.4.0), the gyro still reaches the in-memory checks, and a recovered session stamps the same.
namespace {
struct ImuRun {
  bundle::StreamContents c;
  std::string manifest;
};

// 5 s of walk (30 Hz poses, 200 Hz gyro). killed = no finalize, then ck_recover; `edit_session_json` may rewrite the
// checkpoint in between (to play a session.json written before ADR 0034).
ImuRun imu_run(const char* name, const char* cfg, bool killed = false,
               const std::function<void(const fs::path&)>& edit_session_json = {}) {
  auto d = fresh(name);
  auto* s = make(d, cfg);
  Feeder f{s, d};
  EXPECT_EQ(ck_session_start(s, 0), CK_OK);
  intr(s);
  for (int64_t i = 0; i < 150; ++i) {
    f.step(i);
    ck_session_pump(s);
    f.handle_events();
  }
  if (!killed) {
    EXPECT_EQ(ck_session_stop(s, 150 * 33 * kMs), CK_OK);
    ck_session_pump(s);
    f.handle_events();
    ck_session_pump(s);
    EXPECT_EQ(ck_session_finalize(s), CK_OK);
  }
  ck_session_destroy(s);
  if (killed) {
    if (edit_session_json) edit_session_json(d / "session.json");
    char rep[1024];
    EXPECT_EQ(ck_recover(d.c_str(), rep, sizeof rep), CK_OK) << rep;
  }
  return {bundle::read_streams((d / "streams.mcap").string()), slurp(d / "manifest.json")};
}
}  // namespace

TEST(SessionLeanBundle, ImuIsNotRecordedByDefaultAndTheManifestSaysSo) {
  auto r = imu_run("lean_default", "{\"threaded\":0, \"platform\":\"test\"}");
  EXPECT_TRUE(r.c.finalized);
  EXPECT_TRUE(r.c.gyro.empty());
  EXPECT_TRUE(r.c.accel.empty());
  EXPECT_GT(r.c.poses.size(), 100u);  // everything else is recorded as before
  EXPECT_FALSE(r.c.frames.empty());
  EXPECT_NE(r.manifest.find("\"schema_version\": \"1.4.0\""), std::string::npos) << r.manifest.substr(0, 160);
  EXPECT_NE(r.manifest.find("\"imu_gyro\": 0, \"imu_accel\": 0"), std::string::npos);
  EXPECT_NE(r.manifest.find("\"record_imu\":false"), std::string::npos);
}

TEST(SessionLeanBundle, RecordImuKeepsTheChannelsAndTheOlderSchema) {
  auto r = imu_run("lean_off", "{\"threaded\":0, \"platform\":\"test\", \"record_imu\":1}");
  EXPECT_GT(r.c.gyro.size(), 100u);
  EXPECT_NE(r.manifest.find("\"schema_version\": \"1.1.0\""), std::string::npos) << r.manifest.substr(0, 160);
  EXPECT_NE(r.manifest.find("\"record_imu\":true"), std::string::npos);
  EXPECT_EQ(r.manifest.find("\"imu_gyro\": 0,"), std::string::npos);
}

TEST(SessionLeanBundle, ConsentAndGuideOutputsNeverLowerTheLeanSchema) {
  // 1.2.0 (consent) and 1.3.0 (guide outputs) are older than 1.4.0: the lean stamp wins, the blocks stay.
  auto r = imu_run("lean_consent", "{\"threaded\":0, \"consent_version\":\"v1\", \"consent_at\":\"2026-11-20T09:30:00Z\", "
                                   "\"redaction_mode\":\"none\"}");
  EXPECT_NE(r.manifest.find("\"schema_version\": \"1.4.0\""), std::string::npos);
  EXPECT_NE(r.manifest.find("\"consent\": {\"version\": \"v1\""), std::string::npos);
}

TEST(SessionLeanBundle, RecoveredBundleKeepsTheLeanSchema) {
  auto r = imu_run("lean_killed", "{\"threaded\":0, \"platform\":\"test\"}", /*killed=*/true);
  EXPECT_TRUE(r.c.gyro.empty());
  EXPECT_NE(r.manifest.find("\"schema_version\": \"1.4.0\""), std::string::npos) << r.manifest.substr(0, 160);
  EXPECT_NE(r.manifest.find("recovered"), std::string::npos);
}

TEST(SessionLeanBundle, SessionJsonFromBeforeTheChangeRecoversAsAnImuBundle) {
  // A checkpoint written by an older build has no imu_recorded key: that session recorded the IMU, so no 1.4.0.
  auto r = imu_run("lean_old_checkpoint", "{\"threaded\":0, \"platform\":\"test\", \"record_imu\":1}", /*killed=*/true,
                   [](const fs::path& p) {
                     auto text = slurp(p);
                     const std::string key = "\"imu_recorded\":true,";
                     auto at = text.find(key);
                     ASSERT_NE(at, std::string::npos) << text;
                     text.erase(at, key.size());
                     std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
                   });
  EXPECT_GT(r.c.gyro.size(), 100u);
  EXPECT_NE(r.manifest.find("\"schema_version\": \"1.1.0\""), std::string::npos) << r.manifest.substr(0, 160);
}
