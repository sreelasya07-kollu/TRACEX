// Phase 4 (tests first): quality metrics on generated images, scoring, keyframe selector, config,
// no per-frame heap allocation, desktop timing proxy.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "capturekit/quality/quality.hpp"

// ---- Allocation counter (whole test binary; only read around the call under test) ----
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CK_NO_ALLOC_HOOK 1  // sanitizers own operator new; the allocation test is skipped there
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define CK_NO_ALLOC_HOOK 1
#endif
#endif
static std::atomic<long> g_allocs{0};
#ifndef CK_NO_ALLOC_HOOK
void* operator new(std::size_t n) {
  ++g_allocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

using namespace capturekit::quality;

namespace {

struct Img {
  int w, h;
  std::vector<uint8_t> px;
  LumaView view() const { return {px.data(), w, h, w}; }
};

Img textured(int w = 640, int h = 480, uint32_t seed = 1) {
  Img im{w, h, std::vector<uint8_t>(size_t(w * h))};
  std::mt19937 g(seed);
  std::uniform_int_distribution<int> v(30, 220), s(4, 24);
  // Random rectangles over a mid-grey: strong edges and corners everywhere.
  std::fill(im.px.begin(), im.px.end(), 128);
  for (int k = 0; k < 400; ++k) {
    int x0 = int(g() % uint32_t(w)), y0 = int(g() % uint32_t(h)), bw = s(g), bh = s(g), c = v(g);
    for (int y = y0; y < std::min(h, y0 + bh); ++y)
      for (int x = x0; x < std::min(w, x0 + bw); ++x) im.px[size_t(y * w + x)] = uint8_t(c);
  }
  return im;
}

Img box_blur(const Img& in, int rx, int ry, int passes) {
  Img out = in;
  for (int p = 0; p < passes; ++p) {
    Img tmp = out;
    for (int y = 0; y < in.h; ++y)
      for (int x = 0; x < in.w; ++x) {
        int sum = 0, n = 0;
        for (int dy = -ry; dy <= ry; ++dy)
          for (int dx = -rx; dx <= rx; ++dx) {
            int xx = std::clamp(x + dx, 0, in.w - 1), yy = std::clamp(y + dy, 0, in.h - 1);
            sum += tmp.px[size_t(yy * in.w + xx)];
            ++n;
          }
        out.px[size_t(y * in.w + x)] = uint8_t(sum / n);
      }
  }
  return out;
}

Img affine(const Img& in, double a, double b, int noise_sigma = 0, uint32_t seed = 3) {
  Img out = in;
  std::mt19937 g(seed);
  std::normal_distribution<double> n(0, noise_sigma > 0 ? noise_sigma : 1);
  for (auto& p : out.px) {
    double v = a * p + b + (noise_sigma > 0 ? n(g) : 0);
    p = uint8_t(std::clamp(v, 0.0, 255.0));
  }
  return out;
}

Img flat(int noise_sigma) { return affine(Img{640, 480, std::vector<uint8_t>(640 * 480, 128)}, 1, 0, noise_sigma); }

FrameMetrics measure(FrameAnalyzer& a, const Img& im) {
  FrameMetrics m;
  a.measure(im.view(), m);
  return m;
}

}  // namespace

TEST(QualityMetrics, SharpnessOrdering) {
  FrameAnalyzer a(QualityConfig{});
  auto sharp = textured();
  double s = measure(a, sharp).sharpness;
  double g = measure(a, box_blur(sharp, 2, 2, 3)).sharpness;
  double mb = measure(a, box_blur(sharp, 9, 0, 1)).sharpness;
  double wall = measure(a, flat(2)).sharpness;
  EXPECT_GT(s, 4 * g);
  EXPECT_GT(s, 2 * mb);
  EXPECT_GT(g, wall);
}

TEST(QualityMetrics, ExposureClipping) {
  FrameAnalyzer a(QualityConfig{});
  auto sharp = textured();
  auto over = measure(a, affine(sharp, 0.5, 190));
  auto under = measure(a, affine(sharp, 0.02, 0));
  auto ok = measure(a, sharp);
  EXPECT_GT(over.clip_high, 0.5);
  EXPECT_GT(under.clip_low, 0.5);
  EXPECT_LT(ok.clip_high + ok.clip_low, 0.05);
}

TEST(QualityMetrics, CornersAndSpread) {
  FrameAnalyzer a(QualityConfig{});
  auto sharp = textured();
  auto m = measure(a, sharp);
  EXPECT_GT(m.corners, 100u);
  EXPECT_GT(m.corner_spread, 0.9);
  EXPECT_LT(measure(a, flat(2)).corners, 10u);
  // Texture only in the top-left quadrant -> low spread.
  Img q = flat(0);
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 320; ++x) q.px[size_t(y * 640 + x)] = sharp.px[size_t(y * 640 + x)];
  EXPECT_LT(measure(a, q).corner_spread, 0.4);
}

TEST(QualityMetrics, GyroBlurRisk) {
  ImuRing ring;
  for (int64_t i = 0; i < 100; ++i) ring.push(i * 5'000'000, 2.0, 0.0, 0.0);  // 2 rad/s
  // 20 ms exposure at fx=500 px: 2 * 0.02 * 500 = 20 px.
  EXPECT_NEAR(blur_risk_px(ring, 250'000'000, 20'000'000, 500.0, 16'000'000), 20.0, 1e-6);
  ImuRing still;
  for (int64_t i = 0; i < 100; ++i) still.push(i * 5'000'000, 0.005, 0, 0);
  EXPECT_LT(blur_risk_px(still, 250'000'000, 20'000'000, 500.0, 16'000'000), 0.1);
  // Unknown exposure falls back to the configured default.
  EXPECT_NEAR(blur_risk_px(ring, 250'000'000, 0, 500.0, 10'000'000), 10.0, 1e-6);
  // No gyro in the window -> NaN (unknown), not zero.
  EXPECT_TRUE(std::isnan(blur_risk_px(ImuRing{}, 0, 10'000'000, 500.0, 10'000'000)));
}

TEST(QualityScore, HardRejectsAndSoftPenalties) {
  QualityConfig cfg;
  FrameAnalyzer a(cfg);
  auto sharp = textured();
  // Warm the per-session baseline with sharp frames.
  for (uint32_t k = 0; k < 5; ++k) {
    auto m = measure(a, textured(640, 480, 10 + k));
    m.blur_risk_px = 0.2;
    a.finish(m, 1);
  }
  auto eval = [&](const Img& im, double blur_px, int tracking) {
    auto m = measure(a, im);
    m.blur_risk_px = blur_px;
    a.finish(m, tracking);
    return m;
  };
  auto good = eval(sharp, 0.2, 1);
  EXPECT_FALSE(good.hard_reject);
  EXPECT_GT(good.quality, 0.8);
  EXPECT_EQ(good.reasons, 0u);

  auto blurred = eval(box_blur(sharp, 2, 2, 3), 0.2, 1);
  EXPECT_TRUE(blurred.hard_reject);
  EXPECT_TRUE(blurred.reasons & kBlur);

  auto motion = eval(sharp, 8.0, 1);
  EXPECT_TRUE(motion.hard_reject);
  EXPECT_TRUE(motion.reasons & kMotionBlur);

  auto over = eval(affine(sharp, 0.5, 190), 0.2, 1);
  EXPECT_TRUE(over.hard_reject && (over.reasons & kOverexposed));
  auto under = eval(affine(sharp, 0.02, 0), 0.2, 1);
  EXPECT_TRUE(under.hard_reject && (under.reasons & kUnderexposed));

  auto lost = eval(sharp, 0.2, 3);
  EXPECT_TRUE(lost.hard_reject && (lost.reasons & kTrackingLost));
  auto limited = eval(sharp, 0.2, 2);
  EXPECT_FALSE(limited.hard_reject);
  EXPECT_TRUE(limited.reasons & kTrackingLimited);
  EXPECT_LT(limited.quality, good.quality);

  // Blank wall: low Laplacian is texture, not blur.
  auto wall = eval(flat(2), 0.2, 1);
  EXPECT_TRUE(wall.reasons & kLowTexture);
  EXPECT_FALSE(wall.reasons & kBlur);
  EXPECT_FALSE(wall.hard_reject);

  // Noise must not fake sharpness into a reject or hide a good frame.
  auto noisy = eval(affine(sharp, 1, 0, 12), 0.2, 1);
  EXPECT_FALSE(noisy.hard_reject);
  EXPECT_STREQ(reason_code(kMotionBlur), "MOTION_BLUR");
}

// ---- Keyframe selector ----
namespace {
FrameMetrics good_metrics(double q = 0.9) {
  FrameMetrics m;
  m.quality = q;
  return m;
}
FrameMetrics rejected_metrics() {
  FrameMetrics m;
  m.hard_reject = true;
  m.reasons = kMotionBlur;
  return m;
}
capturekit::geometry::PoseWC at(double x, double yaw_deg = 0) {
  return capturekit::geometry::PoseWC(Sophus::SE3d(
      Eigen::Quaterniond(Eigen::AngleAxisd(yaw_deg * M_PI / 180, Eigen::Vector3d::UnitY())),
      Eigen::Vector3d(x, 0, 0)));
}
constexpr int64_t kMs = 1'000'000;
}  // namespace

TEST(KeyframeSelector, FirstGoodFrameThenNovelty) {
  KeyframeSelector ks(QualityConfig{});
  std::vector<KeyframeSelector::Decision> d;
  ks.push(0, 0, rejected_metrics(), at(0), d);   // rejected
  ks.push(1, 100 * kMs, good_metrics(0.7), at(0), d);  // opens window
  ks.push(2, 200 * kMs, good_metrics(0.9), at(0), d);  // best in window
  ks.push(3, 500 * kMs, good_metrics(0.9), at(0.01), d);  // closes window; not novel
  for (int i = 4; i < 10; ++i) ks.push(uint32_t(i), i * 200 * kMs, good_metrics(), at(0.02), d);
  ks.flush(d);
  std::vector<uint32_t> kf;
  for (auto& x : d) if (x.keyframe) kf.push_back(x.frame_id);
  EXPECT_EQ(kf, std::vector<uint32_t>({2}));
  EXPECT_EQ(d.size(), 10u);  // every frame gets exactly one decision
  for (auto& x : d)
    if (x.frame_id == 0) EXPECT_TRUE(x.reasons & kMotionBlur);
    else if (x.frame_id == 1) EXPECT_TRUE(x.reasons & kNotBest);
    else if (x.frame_id >= 3) EXPECT_TRUE(x.reasons & kNotNovel);
}

TEST(KeyframeSelector, TranslationAndRotationTriggers) {
  QualityConfig cfg;  // default depth 2 m, ratio 0.1 -> 0.2 m; rotation 10 deg
  KeyframeSelector ks(cfg);
  std::vector<KeyframeSelector::Decision> d;
  ks.push(0, 0, good_metrics(), at(0), d);
  ks.push(1, 1000 * kMs, good_metrics(), at(0.25), d);         // translation trigger
  ks.push(2, 2000 * kMs, good_metrics(), at(0.25, 12), d);      // rotation trigger
  ks.push(3, 3000 * kMs, good_metrics(), at(0.26, 13), d);      // not novel
  ks.flush(d);
  int n = 0;
  for (auto& x : d) n += x.keyframe;
  EXPECT_EQ(n, 3);
}

TEST(KeyframeSelector, MinGapAndBestInWindow) {
  KeyframeSelector ks(QualityConfig{});  // window 300 ms, min gap 200 ms
  std::vector<KeyframeSelector::Decision> d;
  ks.push(0, 0, good_metrics(0.5), at(0), d);           // opens window [0, 300]
  ks.push(1, 250 * kMs, good_metrics(0.9), at(0), d);   // best in first window
  ks.push(2, 350 * kMs, good_metrics(), at(1.0), d);    // closes window; novel but 100 ms after kf 1
  ks.push(3, 600 * kMs, good_metrics(0.6), at(1.0), d);  // opens window [600, 900]
  ks.push(4, 700 * kMs, rejected_metrics(), at(1.0), d);
  ks.push(5, 800 * kMs, good_metrics(0.95), at(1.0), d);
  ks.push(6, 1300 * kMs, good_metrics(0.99), at(1.0), d);  // closes window; not novel vs kf 5
  ks.flush(d);
  std::vector<uint32_t> kf;
  for (auto& x : d) if (x.keyframe) kf.push_back(x.frame_id);
  EXPECT_EQ(kf, std::vector<uint32_t>({1, 5}));
  for (auto& x : d) {
    if (x.frame_id == 2) EXPECT_TRUE(x.reasons & kMinGap);
    if (x.frame_id == 3) EXPECT_TRUE(x.reasons & kNotBest);
    if (x.frame_id == 6) EXPECT_TRUE(x.reasons & kNotNovel);
  }
}

// ---- Keyframe selector: novelty against all saved keyframes (issue #9, ADR 0033) ----
namespace {
std::vector<uint32_t> keyframe_ids(const std::vector<KeyframeSelector::Decision>& d) {
  std::vector<uint32_t> kf;
  for (auto& x : d) if (x.keyframe) kf.push_back(x.frame_id);
  return kf;
}
}  // namespace

TEST(KeyframeSelector, RevisitAtoBtoAGivesNoSecondKeyframe) {
  KeyframeSelector ks(QualityConfig{});  // depth 2 m, ratio 0.1 -> 0.2 m
  std::vector<KeyframeSelector::Decision> d;
  constexpr int kLeg = 120;  // 30 fps, 0.5 m/s: A (x = 0) -> B (x = 2) -> A, same viewing direction
  uint32_t id = 0;
  for (int i = 0; i <= kLeg; ++i, ++id) ks.push(id, int64_t(id) * 33 * kMs, good_metrics(), at(2.0 * i / kLeg), d);
  const uint32_t first_return = id;
  for (int i = 1; i <= kLeg; ++i, ++id) ks.push(id, int64_t(id) * 33 * kMs, good_metrics(), at(2.0 - 2.0 * i / kLeg), d);
  ks.flush(d);
  auto kf = keyframe_ids(d);
  ASSERT_GE(kf.size(), 8u);  // outbound leg is covered
  for (auto k : kf) EXPECT_LT(k, first_return) << "revisit frame " << k << " became a second keyframe";
  int dup = 0;
  for (auto& x : d)
    if (x.frame_id >= first_return) {
      // NOT_BEST: the first return frames can fall inside the last outbound candidate window and lose it.
      EXPECT_TRUE(x.reasons & (kDuplicate | kNotNovel | kMinGap | kNotBest)) << x.frame_id;
      dup += (x.reasons & kDuplicate) != 0;
    }
  EXPECT_GT(dup, kLeg / 2);  // most of the way back is a revisit of older keyframes, not the last one
  EXPECT_EQ(d.size(), size_t(id));
}

TEST(KeyframeSelector, Slow360TurnKeepsOnePerRotDegAndSecondLapNone) {
  QualityConfig cfg;  // rot_deg 10
  KeyframeSelector ks(cfg);
  std::vector<KeyframeSelector::Decision> d;
  constexpr double kStep = 0.7;  // deg per frame at 30 fps = 21 deg/s, turning in place
  const int lap = int(std::ceil(360.0 / kStep));
  for (int i = 0; i < 2 * lap; ++i) ks.push(uint32_t(i), int64_t(i) * 33 * kMs, good_metrics(), at(0, kStep * i), d);
  ks.flush(d);
  auto kf = keyframe_ids(d);
  const auto lo = size_t(std::floor(360.0 / (cfg.rot_deg + kStep)));
  const auto hi = size_t(std::ceil(360.0 / cfg.rot_deg));
  EXPECT_GE(kf.size(), lo);
  EXPECT_LE(kf.size(), hi);
  for (auto k : kf) EXPECT_LT(k, uint32_t(lap)) << "second lap frame " << k << " became a keyframe";
  for (size_t i = 1; i < kf.size(); ++i) {
    const double sep = kStep * double(kf[i] - kf[i - 1]);
    EXPECT_GE(sep, cfg.rot_deg - 1e-6);
    EXPECT_LE(sep, cfg.rot_deg + kStep + 1e-6);  // one keyframe per rot_deg, no gaps
  }
  int dup = 0;
  for (auto& x : d) dup += x.frame_id >= uint32_t(lap) && (x.reasons & kDuplicate);
  EXPECT_GT(dup, lap / 2);
}

TEST(KeyframeSelector, BlurredFramesNeverWinTheWindow) {
  KeyframeSelector ks(QualityConfig{});  // window 300 ms
  std::vector<KeyframeSelector::Decision> d;
  auto hard_blur = [] {  // a hard reject must lose even if its quality field were high
    FrameMetrics m = rejected_metrics();
    m.quality = 0.99;
    return m;
  };
  FrameMetrics soft = good_metrics(0.4);
  soft.reasons = kMotionBlur;  // soft blur penalty already folded into quality
  ks.push(0, 0, good_metrics(0.6), at(0), d);         // opens window [0, 300]
  ks.push(1, 100 * kMs, hard_blur(), at(0), d);       // blurred: never a candidate
  ks.push(2, 200 * kMs, soft, at(0), d);              // soft blur: loses to the sharp frame
  for (int i = 3; i < 8; ++i) ks.push(uint32_t(i), i * 400 * kMs, hard_blur(), at(0.5 * i), d);  // novel but blurred
  ks.push(8, 3400 * kMs, good_metrics(0.5), at(4.0), d);  // first sharp novel frame
  ks.flush(d);
  EXPECT_EQ(keyframe_ids(d), std::vector<uint32_t>({0, 8}));
  for (auto& x : d) {
    if (x.frame_id == 1 || (x.frame_id >= 3 && x.frame_id < 8)) {
      EXPECT_FALSE(x.keyframe);
      EXPECT_TRUE(x.reasons & kMotionBlur);
    }
    if (x.frame_id == 2) EXPECT_TRUE(x.reasons & kNotBest);
  }
}

TEST(KeyframeSelector, WindowFrameDuplicatingOlderKeyframeCannotWin) {
  KeyframeSelector ks(QualityConfig{});
  std::vector<KeyframeSelector::Decision> d;
  ks.push(0, 0, good_metrics(), at(0), d);                    // keyframe A
  ks.push(1, 1000 * kMs, good_metrics(), at(1.0), d);         // keyframe B
  ks.push(2, 2000 * kMs, good_metrics(0.5), at(0.4), d);      // novel vs A and B: opens window
  ks.push(3, 2100 * kMs, good_metrics(0.95), at(0.05), d);    // better, but a revisit of A
  ks.push(4, 2200 * kMs, good_metrics(0.9), at(0.95), d);     // better, but a revisit of B (the last keyframe)
  ks.flush(d);
  EXPECT_EQ(keyframe_ids(d), std::vector<uint32_t>({0, 1, 2}));
  for (auto& x : d) {
    if (x.frame_id == 3) EXPECT_TRUE(x.reasons & kDuplicate);
    if (x.frame_id == 4) EXPECT_TRUE(x.reasons & kNotNovel);
  }
}

TEST(KeyframeSelector, PoseIndexMemoryBound) {
  KeyframeSelector ks(QualityConfig{});
  std::vector<KeyframeSelector::Decision> d;
  const size_t n = KeyframeSelector::kPoseIndexCap + 500;
  d.reserve(n + 8);
  ks.push(0, 0, good_metrics(), at(0), d);  // first keyframe; the index storage exists from construction
  long before = g_allocs.load();
  for (size_t i = 1; i < n; ++i) ks.push(uint32_t(i), int64_t(i) * 1000 * kMs, good_metrics(), at(double(i)), d);
  ks.flush(d);
  long allocs = g_allocs.load() - before;
  EXPECT_EQ(ks.keyframes(), n);
  EXPECT_EQ(ks.indexed_keyframes(), KeyframeSelector::kPoseIndexCap);  // ring: newest cap poses kept
#ifndef CK_NO_ALLOC_HOOK
  EXPECT_EQ(allocs, 0);
#endif
  (void)allocs;
  // A recent keyframe pose is still a duplicate; the oldest one was evicted and may be kept again.
  int64_t t = int64_t(n + 1) * 1000 * kMs;
  ks.push(uint32_t(n), t, good_metrics(), at(double(n - 10)), d);
  ks.push(uint32_t(n + 1), t + 1000 * kMs, good_metrics(), at(0), d);
  ks.flush(d);
  bool recent_dup = false, oldest_kept = false;
  for (auto& x : d) {
    if (x.frame_id == uint32_t(n)) recent_dup = !x.keyframe && (x.reasons & kDuplicate);
    if (x.frame_id == uint32_t(n + 1)) oldest_kept = x.keyframe;
  }
  EXPECT_TRUE(recent_dup);
  EXPECT_TRUE(oldest_kept);
  EXPECT_EQ(ks.indexed_keyframes(), KeyframeSelector::kPoseIndexCap);
}

TEST(KeyframeSelector, ReanchorClearsIndexOnlyAboveDedupThresholds) {
  auto run = [](double drift_m, double drift_deg, bool& cleared) {
    KeyframeSelector ks(QualityConfig{});  // 0.2 m at the 2 m default depth, 10 deg
    std::vector<KeyframeSelector::Decision> d;
    ks.push(0, 0, good_metrics(), at(0), d);              // keyframe A
    ks.push(1, 1000 * kMs, good_metrics(), at(1.0), d);   // keyframe B
    ks.push(2, 2000 * kMs, good_metrics(), at(2.0), d);   // keyframe C (closes B's window)
    ks.push(3, 3000 * kMs, good_metrics(), at(3.0), d);   // keyframe D
    cleared = ks.reanchor(drift_m, drift_deg);            // TRACKING_RESET with this drift
    ks.push(4, 4000 * kMs, good_metrics(), at(0.0), d);   // view at A's old pose after the reset
    ks.flush(d);
    for (auto& x : d)
      if (x.frame_id == 4) return x;
    return KeyframeSelector::Decision{};
  };
  bool cleared = false;
  auto big = run(0.5, 0, cleared);  // 0.5 m drift: old poses are in another frame
  EXPECT_TRUE(cleared);
  EXPECT_TRUE(big.keyframe);
  EXPECT_FALSE(big.reasons & kDuplicate);
  auto turned = run(0, 20, cleared);  // 20 deg yaw drift
  EXPECT_TRUE(cleared);
  EXPECT_TRUE(turned.keyframe);
  auto small = run(0.05, 2, cleared);  // below thresholds: index kept, the revisit is still a duplicate
  EXPECT_FALSE(cleared);
  EXPECT_FALSE(small.keyframe);
  EXPECT_TRUE(small.reasons & kDuplicate);
}

TEST(KeyframeSelector, DuplicateReasonCode) {
  EXPECT_STREQ(reason_code(kDuplicate), "DUPLICATE");
  auto codes = reason_codes(kDuplicate | kNotBest);
  EXPECT_EQ(codes, std::vector<std::string>({"NOT_BEST", "DUPLICATE"}));
}

// ---- Config ----
TEST(QualityConfigTest, OverridesHashAndVersion) {
  auto a = QualityConfig::from_json("{}");
  auto b = QualityConfig::from_json("{\"blur_reject_px\": 4.5}");
  ASSERT_TRUE(a && b);
  EXPECT_DOUBLE_EQ(b->blur_reject_px, 4.5);
  EXPECT_NE(a->hash(), b->hash());
  EXPECT_EQ(a->hash(), QualityConfig{}.hash());
  EXPECT_NE(a->to_json().find("\"version\""), std::string::npos);
  std::string err;
  EXPECT_FALSE(QualityConfig::from_json("{\"no_such_key\": 1}", &err));
  EXPECT_NE(err.find("no_such_key"), std::string::npos);
}

// ---- Hot path constraints ----
TEST(QualityPerf, NoHeapAllocationPerFrame) {
#ifdef CK_NO_ALLOC_HOOK
  GTEST_SKIP() << "allocation hook disabled under sanitizers";
#endif
  FrameAnalyzer a(QualityConfig{});
  auto im = textured(1920, 1440);
  FrameMetrics m;
  a.measure(im.view(), m);  // first call may size buffers
  a.finish(m, 1);
  long before = g_allocs.load();
  for (int i = 0; i < 5; ++i) {
    a.measure(im.view(), m);
    a.finish(m, 1);
  }
  EXPECT_EQ(g_allocs.load() - before, 0);
}

TEST(QualityPerf, DesktopProxyTiming) {
  FrameAnalyzer a(QualityConfig{});
  auto im = textured(1920, 1440);
  FrameMetrics m;
  a.measure(im.view(), m);
  const int n = 30;
  auto t0 = std::chrono::steady_clock::now();  // test-only timing; the core never reads clocks
  for (int i = 0; i < n; ++i) {
    a.measure(im.view(), m);
    a.finish(m, 1);
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
  RecordProperty("ms_per_frame_1920x1440", std::to_string(ms));
  std::printf("[ quality ] %.3f ms per 1920x1440 frame\n", ms);
  // Target <= 3 ms (phase 4). Shared CI runners are slower: enforce strictly only on request.
  const bool strict = std::getenv("CK_PERF_STRICT") != nullptr;
#if defined(NDEBUG) && !defined(CK_NO_ALLOC_HOOK)
  EXPECT_LT(ms, strict ? 3.0 : 15.0);
#endif
}
