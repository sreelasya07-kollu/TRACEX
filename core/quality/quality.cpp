#include "capturekit/quality/quality.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "capturekit/bundle/finalize.hpp"
#include "capturekit/util/json.hpp"

namespace capturekit::quality {

const char* reason_code(Reason r) {
  switch (r) {
    case kTrackingLost: return "TRACKING_LOST";
    case kTrackingLimited: return "TRACKING_LIMITED";
    case kMotionBlur: return "MOTION_BLUR";
    case kBlur: return "BLUR";
    case kSoftFocus: return "SOFT_FOCUS";
    case kOverexposed: return "OVEREXPOSED";
    case kUnderexposed: return "UNDEREXPOSED";
    case kLowTexture: return "LOW_TEXTURE";
    case kPoorSpread: return "POOR_SPREAD";
    case kNoImage: return "NO_IMAGE";
    case kNotNovel: return "NOT_NOVEL";
    case kMinGap: return "MIN_GAP";
    case kNotBest: return "NOT_BEST";
    case kRecalibrating: return "RECALIBRATING";
    case kDuplicate: return "DUPLICATE";
  }
  return "UNKNOWN";
}

std::vector<std::string> reason_codes(uint32_t mask) {
  std::vector<std::string> v;
  for (uint32_t b = 1; b && b <= kLastReason; b <<= 1)
    if (mask & b) v.emplace_back(reason_code(Reason(b)));
  return v;
}

// ---- Config ----
namespace {
template <class F>
void for_each_field(QualityConfig& c, F&& f) {
  f("target_width", c.target_width); f("fast_threshold", c.fast_threshold);
  f("corner_cell_min", c.corner_cell_min); f("min_corners", c.min_corners);
  f("min_spread", c.min_spread); f("texture_min", c.texture_min);
  f("sharp_reject", c.sharp_reject); f("sharp_soft", c.sharp_soft);
  f("baseline_up", c.baseline_up); f("baseline_down", c.baseline_down);
  f("clip_soft", c.clip_soft); f("clip_reject", c.clip_reject);
  f("blur_soft_px", c.blur_soft_px); f("blur_reject_px", c.blur_reject_px);
  f("default_exposure_ms", c.default_exposure_ms); f("limited_penalty", c.limited_penalty);
  f("scene_depth_m", c.scene_depth_m); f("trans_ratio", c.trans_ratio); f("rot_deg", c.rot_deg);
  f("min_gap_ms", c.min_gap_ms); f("window_ms", c.window_ms);
}
}  // namespace

std::optional<QualityConfig> QualityConfig::from_json(std::string_view text, std::string* err) {
  auto obj = json::parse_flat(text, err);
  if (!obj) return std::nullopt;
  QualityConfig c;
  for (auto& [k, v] : *obj) {
    bool found = false;
    for_each_field(c, [&](const char* name, double& field) {
      if (k == name) {
        found = true;
        if (auto* d = std::get_if<double>(&v)) field = *d;
      }
    });
    if (!found) {
      if (err) *err = "unknown quality config key: " + k;
      return std::nullopt;
    }
  }
  return c;
}

std::string QualityConfig::to_json() const {
  json::Object o;
  auto c = *this;
  for_each_field(c, [&](const char* name, double& field) { o.set(name, field); });
  o.set("version", version);
  return o.dump();
}

std::string QualityConfig::hash() const {
  auto j = to_json();
  return bundle::sha256_hex_bytes(j.data(), j.size()).substr(0, 16);
}

// ---- Gyro ----
void ImuRing::push(int64_t t, double wx, double wy, double wz) {
  t_[head_] = t;
  w_[head_] = std::sqrt(wx * wx + wy * wy + wz * wz);
  head_ = (head_ + 1) % kCap;
  n_ = std::min(n_ + 1, kCap);
}

double ImuRing::mean_rate(int64_t t0, int64_t t1) const {
  double sum = 0;
  int n = 0;
  int64_t best_d = 10'000'000;
  double nearest = std::numeric_limits<double>::quiet_NaN();
  int64_t mid = t0 + (t1 - t0) / 2;
  for (size_t i = 0; i < n_; ++i) {
    size_t k = (head_ + kCap - 1 - i) % kCap;
    if (t_[k] >= t0 && t_[k] <= t1) {
      sum += w_[k];
      ++n;
    }
    int64_t d = std::llabs(t_[k] - mid);
    if (d <= best_d) best_d = d, nearest = w_[k];
    if (t_[k] < t0 - 10'000'000) break;  // samples are time-ordered; older ones are irrelevant
  }
  return n ? sum / n : nearest;
}

double blur_risk_px(const ImuRing& g, int64_t t, int64_t exposure_ns, double focal_px,
                    int64_t default_exposure_ns) {
  int64_t e = exposure_ns > 0 ? exposure_ns : default_exposure_ns;
  double w = g.mean_rate(t - e / 2, t + e / 2);
  if (std::isnan(w)) return w;
  return w * (double(e) / 1e9) * focal_px;
}

// ---- Image metrics ----
namespace {

// Row-accumulating box filter: sums f rows into per-column accumulators (vectorises well).
// acc must hold dw ints; no allocation here.
void box_downscale(const uint8_t* src, int stride, int f, uint8_t* dst, int dw, int dh, uint32_t* acc) {
  const uint32_t area = uint32_t(f * f);
  for (int y = 0; y < dh; ++y) {
    std::fill(acc, acc + dw * f, 0u);
    for (int yy = 0; yy < f; ++yy) {
      const uint8_t* row = src + size_t(y * f + yy) * size_t(stride);
      for (int x = 0; x < dw * f; ++x) acc[x] += row[x];
    }
    uint8_t* out = dst + size_t(y) * size_t(dw);
    for (int x = 0; x < dw; ++x) {
      uint32_t sum = 0;
      for (int k = 0; k < f; ++k) sum += acc[x * f + k];
      out[x] = uint8_t(sum / area);
    }
  }
}

double laplacian_var(const uint8_t* p, int w, int h) {
  if (w < 3 || h < 3) return 0;
  double s = 0, s2 = 0;
  const double n = double(w - 2) * double(h - 2);
  for (int y = 1; y < h - 1; ++y) {
    const uint8_t* r = p + size_t(y) * size_t(w);
    for (int x = 1; x < w - 1; ++x) {
      int l = 4 * r[x] - r[x - 1] - r[x + 1] - r[x - w] - r[x + w];
      s += l;
      s2 += double(l) * l;
    }
  }
  double mean = s / n;
  return s2 / n - mean * mean;
}

// FAST-9 segment test (no non-max suppression).
constexpr int kCircle[16][2] = {{0, -3}, {1, -3}, {2, -2}, {3, -1}, {3, 0},  {3, 1},  {2, 2},  {1, 3},
                                {0, 3},  {-1, 3}, {-2, 2}, {-3, 1}, {-3, 0}, {-3, -1}, {-2, -2}, {-1, -3}};

bool is_corner(const uint8_t* p, int w, int x, int y, int t) {
  const int c = p[y * w + x];
  auto px = [&](int i) { return int(p[(y + kCircle[i][1]) * w + x + kCircle[i][0]]); };
  int hi = 0, lo = 0;
  for (int i : {0, 4, 8, 12}) {
    int v = px(i);
    hi += v > c + t;
    lo += v < c - t;
  }
  if (hi < 2 && lo < 2) return false;
  for (int sign : {1, -1}) {
    int run = 0;
    for (int i = 0; i < 16 + 9; ++i) {
      int v = px(i % 16);
      bool pass = sign > 0 ? v > c + t : v < c - t;
      run = pass ? run + 1 : 0;
      if (run >= 9) return true;
    }
  }
  return false;
}

}  // namespace

void FrameAnalyzer::measure(const LumaView& img, FrameMetrics& m) {
  if (!img.data || img.width < 8 || img.height < 8) {
    m.reasons |= kNoImage;
    return;
  }
  const int f = std::max(1, int(std::lround(double(img.width) / cfg_.target_width)));
  const int sw = img.width / f, sh = img.height / f;
  if (sw != sw_ || sh != sh_) {  // allocation only when the resolution changes
    sw_ = sw, sh_ = sh;
    small_.assign(size_t(sw) * size_t(sh), 0);
    cw_ = sw / 4, ch_ = sh / 4;
    coarse_.assign(size_t(std::max(1, cw_)) * size_t(std::max(1, ch_)), 0);
    acc_.assign(size_t(img.width), 0);
  }
  box_downscale(img.data, img.stride, f, small_.data(), sw, sh, acc_.data());
  box_downscale(small_.data(), sw, 4, coarse_.data(), cw_, ch_, acc_.data());

  m.sharpness = laplacian_var(small_.data(), sw, sh);
  m.texture = laplacian_var(coarse_.data(), cw_, ch_);

  uint64_t lo = 0, hi = 0, sum = 0;
  for (uint8_t v : small_) {
    lo += v <= 5;
    hi += v >= 250;
    sum += v;
  }
  const double n = double(small_.size());
  m.clip_low = double(lo) / n;
  m.clip_high = double(hi) / n;
  m.mean_luma = double(sum) / n;

  std::array<uint32_t, 16> cells{};
  uint32_t corners = 0;
  const int t = int(cfg_.fast_threshold);
  for (int y = 3; y < sh - 3; ++y)
    for (int x = 3; x < sw - 3; ++x)
      if (is_corner(small_.data(), sw, x, y, t)) {
        ++corners;
        ++cells[size_t((y * 4 / sh) * 4 + (x * 4 / sw))];
      }
  int occupied = 0;
  for (auto c : cells) occupied += c >= uint32_t(cfg_.corner_cell_min);
  m.corners = corners;
  m.corner_spread = occupied / 16.0;
}

void FrameAnalyzer::finish(FrameMetrics& m, int tracking_state) {
  const auto& c = cfg_;
  m.tracking_state = tracking_state;
  double q = 1.0;
  uint32_t r = m.reasons & kNoImage;
  bool hard = false;

  if (tracking_state == 3) hard = true, r |= kTrackingLost;
  else if (tracking_state == 2) q *= c.limited_penalty, r |= kTrackingLimited;

  if (!std::isnan(m.blur_risk_px)) {
    if (m.blur_risk_px > c.blur_reject_px) hard = true, r |= kMotionBlur;
    else if (m.blur_risk_px > c.blur_soft_px) {
      r |= kMotionBlur;
      q *= 1.0 - 0.5 * (m.blur_risk_px - c.blur_soft_px) / (c.blur_reject_px - c.blur_soft_px);
    }
  }

  bool exposure_ok = true;
  if (!(r & kNoImage)) {
    for (auto [frac, bit] : {std::pair{m.clip_high, kOverexposed}, std::pair{m.clip_low, kUnderexposed}}) {
      if (frac >= c.clip_reject) hard = true, r |= bit, exposure_ok = false;
      else if (frac >= c.clip_soft) q *= 1.0 - frac, r |= bit;
    }

    const bool textured = m.texture >= c.texture_min;
    const double ratio = textured ? m.sharpness / std::max(m.texture, 1e-9) : 0;
    if (textured && baseline_ > 0) m.sharpness_norm = ratio / baseline_;
    else m.sharpness_norm = textured ? 1.0 : 0.0;

    if (!textured) {
      r |= kLowTexture;
      q *= 0.5;
    } else if (m.sharpness_norm < c.sharp_reject) {
      hard = true, r |= kBlur;
    } else if (m.sharpness_norm < c.sharp_soft) {
      r |= kSoftFocus;
      q *= m.sharpness_norm / c.sharp_soft;
    }
    if (m.corners < uint32_t(c.min_corners)) {
      if (!(r & kLowTexture)) q *= 0.7;
      r |= kLowTexture;
    }
    if (m.corner_spread < c.min_spread) r |= kPoorSpread, q *= 0.5 + m.corner_spread;

    // Baseline follows the upper envelope of the sharpness ratio on usable textured frames.
    const bool motion_ok = std::isnan(m.blur_risk_px) || m.blur_risk_px <= c.blur_soft_px;
    if (textured && exposure_ok && motion_ok && tracking_state != 3) {
      if (baseline_ <= 0) baseline_ = ratio;
      else baseline_ += (ratio > baseline_ ? c.baseline_up : c.baseline_down) * (ratio - baseline_);
    }
  }

  m.reasons = r;
  m.hard_reject = hard || (r & kNoImage);
  m.quality = m.hard_reject ? 0.0 : std::clamp(q, 0.0, 1.0);
}

std::optional<double> median_point_depth(const geometry::PoseWC& T, const float* xyz, size_t n) {
  if (n < 10) return std::nullopt;
  std::vector<double> z;
  z.reserve(n);
  auto inv = T.se3().inverse();
  for (size_t i = 0; i < n; ++i) {
    Eigen::Vector3d pc = inv * Eigen::Vector3d(xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]);
    if (pc.z() > 0.2) z.push_back(pc.z());
  }
  if (z.size() < 10) return std::nullopt;
  std::nth_element(z.begin(), z.begin() + long(z.size() / 2), z.end());
  return z[z.size() / 2];
}

// ---- Keyframe selector ----
// A saved keyframe covers T when it is within trans_ratio x depth AND rot_deg of it (ADR 0033). Checked
// against the last keyframe first (NOT_NOVEL, as before), then against every indexed keyframe (DUPLICATE):
// a revisit of an older view is skipped, never saved again. Brute force over the bounded ring: <= 2048 poses,
// cheap translation test first, rotation only for the few nearby.
uint32_t KeyframeSelector::novelty(const geometry::PoseWC& T) const {
  const double depth = depth_ > 0 ? depth_ : cfg_.scene_depth_m;
  const double r = cfg_.trans_ratio * depth;
  const Eigen::Vector3d t = T.translation();
  const Eigen::Quaterniond q = T.rotation();
  auto covers = [&](const Eigen::Vector3d& kt, const Eigen::Quaterniond& kq) {
    return (t - kt).norm() / depth < cfg_.trans_ratio && kq.angularDistance(q) * 180.0 / M_PI < cfg_.rot_deg;
  };
  if (covers(kf_pose_.translation(), kf_pose_.rotation())) return kNotNovel;
  const double r2 = r * r * (1 + 1e-9);  // prefilter only; covers() decides at the boundary
  for (const auto& k : index_)
    if ((t - k.t).squaredNorm() < r2 && covers(k.t, k.q)) return kDuplicate;
  return 0;
}

void KeyframeSelector::clear_index() {
  index_.clear();  // keeps the reserved capacity
  index_head_ = 0;
  have_kf_ = false;
}

bool KeyframeSelector::reanchor(double drift_m, double drift_deg) {
  const double depth = depth_ > 0 ? depth_ : cfg_.scene_depth_m;
  if (!(drift_m / depth >= cfg_.trans_ratio || drift_deg >= cfg_.rot_deg)) return false;
  clear_index();
  return true;
}

void KeyframeSelector::close_window(std::vector<Decision>& out) {
  for (auto& d : pending_) out.push_back(d);
  pending_.clear();
  best_.d.keyframe = true;
  out.push_back(best_.d);
  kf_pose_ = best_.pose;
  kf_t_ = best_.d.t_ns;
  have_kf_ = true;
  window_ = false;
  ++n_kf_;
  const KfPose k{kf_pose_.translation(), kf_pose_.rotation()};
  if (index_.size() < kPoseIndexCap) {
    index_.push_back(k);  // capacity reserved in the constructor: no allocation
  } else {
    index_[index_head_] = k;  // full: overwrite the oldest
    index_head_ = (index_head_ + 1) % kPoseIndexCap;
  }
}

void KeyframeSelector::push(uint32_t id, int64_t t, const FrameMetrics& m, const geometry::PoseWC& T,
                            std::vector<Decision>& out) {
  const int64_t ms = 1'000'000;
  if (window_ && t > window_t0_ + int64_t(cfg_.window_ms * ms)) close_window(out);
  Decision d{id, t, false, m.quality, m.reasons};
  if (m.hard_reject) {
    out.push_back(d);
    return;
  }
  if (window_) {
    // A better frame that a saved keyframe already covers must not win (no duplicate saved via the window).
    const uint32_t covered = d.quality > best_.d.quality && have_kf_ ? novelty(T) : 0;
    if (covered) {
      d.reasons |= covered;
      pending_.push_back(d);
    } else if (d.quality > best_.d.quality) {
      best_.d.reasons |= kNotBest;
      pending_.push_back(best_.d);
      best_ = {d, T};
    } else {
      d.reasons |= kNotBest;
      pending_.push_back(d);
    }
    return;
  }
  if (rate_scale_ <= 0) {
    d.reasons |= kMinGap;
    out.push_back(d);
    return;
  }
  if (have_kf_) {
    if (t - kf_t_ < int64_t(cfg_.min_gap_ms / rate_scale_ * ms)) {
      d.reasons |= kMinGap;
      out.push_back(d);
      return;
    }
    if (const uint32_t covered = novelty(T)) {
      d.reasons |= covered;
      out.push_back(d);
      return;
    }
  }
  window_ = true;
  window_t0_ = t;
  best_ = {d, T};
}

void KeyframeSelector::flush(std::vector<Decision>& out) {
  if (window_) close_window(out);
}

}  // namespace capturekit::quality
