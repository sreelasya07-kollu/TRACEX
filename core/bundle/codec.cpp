#include "capturekit/bundle/codec.hpp"

#include <cmath>
#include <cstring>

namespace capturekit::bundle::codec {
namespace {

class Enc {
 public:
  std::vector<std::byte> out;
  void varint(uint64_t v) {
    while (v >= 0x80) { out.push_back(std::byte(uint8_t(v) | 0x80)); v >>= 7; }
    out.push_back(std::byte(uint8_t(v)));
  }
  void tag(int field, int wire) { varint((uint64_t(field) << 3) | uint64_t(wire)); }
  void i64(int f, int64_t v) { if (v) { tag(f, 0); varint(uint64_t(v)); } }
  void u32(int f, uint32_t v) { if (v) { tag(f, 0); varint(v); } }
  void boolean(int f, bool v) { if (v) { tag(f, 0); varint(1); } }
  void dbl(int f, double v) {
    if (v == 0.0 && !std::signbit(v)) return;
    tag(f, 1);
    uint64_t b; std::memcpy(&b, &v, 8);
    for (int i = 0; i < 8; ++i) out.push_back(std::byte(uint8_t(b >> (8 * i))));
  }
  void flt(int f, float v) {
    if (v == 0.0f && !std::signbit(v)) return;
    tag(f, 5);
    uint32_t b; std::memcpy(&b, &v, 4);
    for (int i = 0; i < 4; ++i) out.push_back(std::byte(uint8_t(b >> (8 * i))));
  }
  void str(int f, std::string_view s) {
    if (s.empty()) return;
    tag(f, 2); varint(s.size());
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    out.insert(out.end(), p, p + s.size());
  }
  void str_always(int f, std::string_view s) {  // repeated string elements must be emitted even if empty
    tag(f, 2); varint(s.size());
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    out.insert(out.end(), p, p + s.size());
  }
  void sub(int f, const Enc& e) { tag(f, 2); varint(e.out.size()); out.insert(out.end(), e.out.begin(), e.out.end()); }
  void packed_dbl(int f, const std::vector<double>& v) {
    if (v.empty()) return;
    tag(f, 2); varint(v.size() * 8);
    for (double d : v) { uint64_t b; std::memcpy(&b, &d, 8); for (int i = 0; i < 8; ++i) out.push_back(std::byte(uint8_t(b >> (8 * i)))); }
  }
  void packed_flt(int f, const std::vector<float>& v) {
    if (v.empty()) return;
    tag(f, 2); varint(v.size() * 4);
    for (float d : v) { uint32_t b; std::memcpy(&b, &d, 4); for (int i = 0; i < 4; ++i) out.push_back(std::byte(uint8_t(b >> (8 * i)))); }
  }
};

struct Field {
  int num = 0, wire = 0;
  uint64_t v = 0;               // varint / fixed64 / fixed32 bits
  const std::byte* p = nullptr;  // length-delimited payload
  size_t n = 0;
};

class Dec {
 public:
  Dec(const std::byte* p, size_t n) : p_(p), end_(p + n) {}
  bool ok() const { return ok_; }
  bool next(Field& f) {
    if (p_ >= end_ || !ok_) return false;
    uint64_t key;
    if (!varint(key)) return fail();
    f.num = int(key >> 3); f.wire = int(key & 7);
    if (f.num <= 0) return fail();
    switch (f.wire) {
      case 0: if (!varint(f.v)) return fail(); break;
      case 1: if (end_ - p_ < 8) return fail(); f.v = le(8); break;
      case 5: if (end_ - p_ < 4) return fail(); f.v = le(4); break;
      case 2: {
        uint64_t len; if (!varint(len)) return fail();
        if (len > uint64_t(end_ - p_)) return fail();
        f.p = p_; f.n = size_t(len); p_ += len; break;
      }
      default: return fail();
    }
    return true;
  }
 private:
  bool fail() { ok_ = false; return false; }
  bool varint(uint64_t& v) {
    v = 0;
    for (int s = 0; s < 64; s += 7) {
      if (p_ >= end_) return false;
      uint8_t b = uint8_t(*p_++);
      v |= uint64_t(b & 0x7f) << s;
      if (!(b & 0x80)) return true;
    }
    return false;
  }
  uint64_t le(int k) { uint64_t v = 0; for (int i = 0; i < k; ++i) v |= uint64_t(uint8_t(p_[i])) << (8 * i); p_ += k; return v; }
  const std::byte* p_;
  const std::byte* end_;
  bool ok_ = true;
};

double as_dbl(const Field& f) { double d; std::memcpy(&d, &f.v, 8); return d; }
float as_flt(uint32_t b) { float d; std::memcpy(&d, &b, 4); return d; }
std::string as_str(const Field& f) { return std::string(reinterpret_cast<const char*>(f.p), f.n); }
bool is_d(const Field& f) { return f.wire == 1; }
bool is_v(const Field& f) { return f.wire == 0; }
bool is_l(const Field& f) { return f.wire == 2; }

// Proto3 omits zero values, so decoding starts from all-zero fields (not the C++ defaults).
template <class T> T zeroed() { return T{}; }
template <> PoseSample zeroed<PoseSample>() {
  PoseSample m; m.q_world_from_camera_wxyz[0] = 0; m.tracking_state = TrackingState::Unspecified; return m;
}
template <> PlaneRecord zeroed<PlaneRecord>() {
  PlaneRecord m; m.normal[1] = 0; m.axis_u[0] = 0; return m;
}

template <class T, class Fn>
std::optional<T> run(const std::byte* p, size_t n, Fn fn) {
  T m = zeroed<T>();
  Dec d(p, n);
  Field f;
  while (d.next(f)) if (!fn(m, f)) return std::nullopt;
  if (!d.ok()) return std::nullopt;
  return m;
}

}  // namespace

std::vector<std::byte> encode(const PoseSample& m) {
  Enc e;
  e.i64(1, m.t_ns);
  for (int i = 0; i < 3; ++i) e.dbl(2 + i, m.t_world_from_camera[i]);
  for (int i = 0; i < 4; ++i) e.dbl(5 + i, m.q_world_from_camera_wxyz[i]);
  e.i64(9, int(m.tracking_state));
  e.i64(10, int(m.tracking_reason));
  return std::move(e.out);
}

std::vector<std::byte> encode(const ImuSample& m) {
  Enc e;
  e.i64(1, m.t_ns);
  for (int i = 0; i < 3; ++i) e.dbl(2 + i, m.v[i]);
  return std::move(e.out);
}

std::vector<std::byte> encode(const IntrinsicsSample& m) {
  Enc e;
  e.i64(1, m.t_ns); e.u32(2, m.width); e.u32(3, m.height);
  e.dbl(4, m.fx); e.dbl(5, m.fy); e.dbl(6, m.cx); e.dbl(7, m.cy);
  e.i64(8, int(m.model));
  e.packed_dbl(9, m.distortion);
  return std::move(e.out);
}

std::vector<std::byte> encode(const FrameRecord& m) {
  Enc e;
  e.i64(1, m.t_ns); e.u32(2, m.frame_id); e.str(3, m.file); e.boolean(4, m.keyframe);
  e.i64(5, m.exposure_ns); e.dbl(6, m.sharpness); e.dbl(7, m.sharpness_norm); e.dbl(8, m.blur_risk_px);
  e.dbl(9, m.clip_low); e.dbl(10, m.clip_high); e.u32(11, m.corners); e.dbl(12, m.corner_spread);
  e.dbl(13, m.quality);
  for (auto& r : m.reasons) e.str_always(14, r);
  e.i64(15, int(m.tracking_state));
  return std::move(e.out);
}

std::vector<std::byte> encode(const EventRecord& m) {
  Enc e;
  e.i64(1, m.t_ns); e.str(2, m.source); e.str(3, m.code); e.str(4, m.message);
  for (auto& [k, v] : m.attrs) { Enc kv; kv.str(1, k); kv.str(2, v); e.sub(5, kv); }
  return std::move(e.out);
}

std::vector<std::byte> encode(const PointCloudRecord& m) {
  Enc e;
  e.i64(1, m.t_ns); e.packed_flt(2, m.xyz);
  return std::move(e.out);
}

std::vector<std::byte> encode(const PlaneRecord& m) {
  Enc e;
  e.i64(1, m.t_ns); e.str(2, m.id); e.str(3, m.kind);
  for (int i = 0; i < 3; ++i) e.dbl(4 + i, m.center[i]);
  for (int i = 0; i < 3; ++i) e.dbl(7 + i, m.normal[i]);
  for (int i = 0; i < 3; ++i) e.dbl(10 + i, m.axis_u[i]);
  e.dbl(13, m.extent_u); e.dbl(14, m.extent_v);
  return std::move(e.out);
}

std::vector<std::byte> encode(const DetectionsRecord& m) {
  Enc e;
  e.i64(1, m.t_ns); e.u32(2, m.frame_id);
  for (auto& b : m.boxes) {
    Enc d;
    d.i64(1, int64_t(b.cls)); d.flt(2, b.score); d.flt(3, b.x0); d.flt(4, b.y0); d.flt(5, b.x1); d.flt(6, b.y1);
    e.sub(3, d);  // repeated message: emitted even when all fields are zero (empty payload)
  }
  return std::move(e.out);
}

std::vector<std::byte> encode(const LabelsRecord& m) {
  Enc e;
  e.i64(1, m.t_ns);
  for (auto& o : m.objects) {
    Enc d;
    d.u32(1, o.id); d.i64(2, int64_t(o.cls));
    for (int i = 0; i < 3; ++i) d.dbl(3 + i, o.centre[i]);
    for (int i = 0; i < 3; ++i) d.dbl(6 + i, o.size[i]);
    d.dbl(9, o.yaw); d.dbl(10, o.conf); d.u32(11, o.n_obs);
    e.sub(2, d);
  }
  for (auto& o : m.openings) {
    Enc d;
    d.i64(1, int64_t(o.kind)); d.str(2, o.wall);
    d.dbl(3, o.x0); d.dbl(4, o.z0); d.dbl(5, o.x1); d.dbl(6, o.z1); d.dbl(7, o.y0); d.dbl(8, o.y1); d.dbl(9, o.conf);
    e.sub(3, d);
  }
  return std::move(e.out);
}

std::vector<std::byte> encode(const ShellRecord& m) {
  Enc e;
  e.i64(1, m.t_ns);
  for (auto& w : m.walls) {
    Enc d;
    d.str(1, w.id); d.dbl(2, w.x0); d.dbl(3, w.z0); d.dbl(4, w.x1); d.dbl(5, w.z1); d.boolean(6, w.inferred);
    e.sub(2, d);
  }
  e.packed_dbl(3, m.corners);
  e.boolean(4, m.closed); e.boolean(5, m.low_confidence); e.dbl(6, m.ceiling_h); e.boolean(7, m.ceiling_inferred);
  return std::move(e.out);
}

std::vector<std::byte> encode(const GapsRecord& m) {
  Enc e;
  e.i64(1, m.t_ns);
  for (auto& g : m.gaps) {
    Enc d;
    d.i64(1, int64_t(g.type)); d.dbl(2, g.x0); d.dbl(3, g.z0); d.dbl(4, g.x1); d.dbl(5, g.z1); d.dbl(6, g.pri); d.str(7, g.why);
    e.sub(2, d);
  }
  return std::move(e.out);
}

std::optional<PoseSample> decode_pose(const std::byte* p, size_t n) {
  auto r = run<PoseSample>(p, n, [](PoseSample& m, const Field& f) {
    if (f.num == 1 && is_v(f)) m.t_ns = int64_t(f.v);
    else if (f.num >= 2 && f.num <= 4 && is_d(f)) m.t_world_from_camera[f.num - 2] = as_dbl(f);
    else if (f.num >= 5 && f.num <= 8 && is_d(f)) m.q_world_from_camera_wxyz[f.num - 5] = as_dbl(f);
    else if (f.num == 9 && is_v(f)) m.tracking_state = TrackingState(int(f.v));
    else if (f.num == 10 && is_v(f)) m.tracking_reason = TrackingReason(int(f.v));
    return true;
  });
  return r;
}

std::optional<ImuSample> decode_imu(const std::byte* p, size_t n) {
  return run<ImuSample>(p, n, [](ImuSample& m, const Field& f) {
    if (f.num == 1 && is_v(f)) m.t_ns = int64_t(f.v);
    else if (f.num >= 2 && f.num <= 4 && is_d(f)) m.v[f.num - 2] = as_dbl(f);
    return true;
  });
}

std::optional<IntrinsicsSample> decode_intrinsics(const std::byte* p, size_t n) {
  return run<IntrinsicsSample>(p, n, [](IntrinsicsSample& m, const Field& f) {
    switch (f.num) {
      case 1: if (is_v(f)) m.t_ns = int64_t(f.v); break;
      case 2: if (is_v(f)) m.width = uint32_t(f.v); break;
      case 3: if (is_v(f)) m.height = uint32_t(f.v); break;
      case 4: if (is_d(f)) m.fx = as_dbl(f); break;
      case 5: if (is_d(f)) m.fy = as_dbl(f); break;
      case 6: if (is_d(f)) m.cx = as_dbl(f); break;
      case 7: if (is_d(f)) m.cy = as_dbl(f); break;
      case 8: if (is_v(f)) m.model = CameraModelId(int(f.v)); break;
      case 9:
        if (is_l(f)) {
          if (f.n % 8) return false;
          for (size_t i = 0; i < f.n; i += 8) { Field g; std::memcpy(&g.v, f.p + i, 8); m.distortion.push_back(as_dbl(g)); }
        } else if (is_d(f)) m.distortion.push_back(as_dbl(f));
        break;
      default: break;
    }
    return true;
  });
}

std::optional<FrameRecord> decode_frame(const std::byte* p, size_t n) {
  return run<FrameRecord>(p, n, [](FrameRecord& m, const Field& f) {
    switch (f.num) {
      case 1: if (is_v(f)) m.t_ns = int64_t(f.v); break;
      case 2: if (is_v(f)) m.frame_id = uint32_t(f.v); break;
      case 3: if (is_l(f)) m.file = as_str(f); break;
      case 4: if (is_v(f)) m.keyframe = f.v != 0; break;
      case 5: if (is_v(f)) m.exposure_ns = int64_t(f.v); break;
      case 6: if (is_d(f)) m.sharpness = as_dbl(f); break;
      case 7: if (is_d(f)) m.sharpness_norm = as_dbl(f); break;
      case 8: if (is_d(f)) m.blur_risk_px = as_dbl(f); break;
      case 9: if (is_d(f)) m.clip_low = as_dbl(f); break;
      case 10: if (is_d(f)) m.clip_high = as_dbl(f); break;
      case 11: if (is_v(f)) m.corners = uint32_t(f.v); break;
      case 12: if (is_d(f)) m.corner_spread = as_dbl(f); break;
      case 13: if (is_d(f)) m.quality = as_dbl(f); break;
      case 14: if (is_l(f)) m.reasons.push_back(as_str(f)); break;
      case 15: if (is_v(f)) m.tracking_state = TrackingState(int(f.v)); break;
      default: break;
    }
    return true;
  });
}

std::optional<EventRecord> decode_event(const std::byte* p, size_t n) {
  return run<EventRecord>(p, n, [](EventRecord& m, const Field& f) {
    switch (f.num) {
      case 1: if (is_v(f)) m.t_ns = int64_t(f.v); break;
      case 2: if (is_l(f)) m.source = as_str(f); break;
      case 3: if (is_l(f)) m.code = as_str(f); break;
      case 4: if (is_l(f)) m.message = as_str(f); break;
      case 5: {
        if (!is_l(f)) break;
        std::pair<std::string, std::string> kv;
        Dec d(f.p, f.n); Field g;
        while (d.next(g)) { if (g.num == 1 && is_l(g)) kv.first = as_str(g); else if (g.num == 2 && is_l(g)) kv.second = as_str(g); }
        if (!d.ok()) return false;
        m.attrs.push_back(std::move(kv));
        break;
      }
      default: break;
    }
    return true;
  });
}

std::optional<PointCloudRecord> decode_points(const std::byte* p, size_t n) {
  return run<PointCloudRecord>(p, n, [](PointCloudRecord& m, const Field& f) {
    if (f.num == 1 && is_v(f)) m.t_ns = int64_t(f.v);
    else if (f.num == 2 && is_l(f)) {
      if (f.n % 4) return false;
      for (size_t i = 0; i < f.n; i += 4) { uint32_t b; std::memcpy(&b, f.p + i, 4); m.xyz.push_back(as_flt(b)); }
    } else if (f.num == 2 && f.wire == 5) m.xyz.push_back(as_flt(uint32_t(f.v)));
    return true;
  });
}

std::optional<PlaneRecord> decode_plane(const std::byte* p, size_t n) {
  return run<PlaneRecord>(p, n, [](PlaneRecord& m, const Field& f) {
    if (f.num == 1 && is_v(f)) m.t_ns = int64_t(f.v);
    else if (f.num == 2 && is_l(f)) m.id = as_str(f);
    else if (f.num == 3 && is_l(f)) m.kind = as_str(f);
    else if (f.num >= 4 && f.num <= 6 && is_d(f)) m.center[f.num - 4] = as_dbl(f);
    else if (f.num >= 7 && f.num <= 9 && is_d(f)) m.normal[f.num - 7] = as_dbl(f);
    else if (f.num >= 10 && f.num <= 12 && is_d(f)) m.axis_u[f.num - 10] = as_dbl(f);
    else if (f.num == 13 && is_d(f)) m.extent_u = as_dbl(f);
    else if (f.num == 14 && is_d(f)) m.extent_v = as_dbl(f);
    return true;
  });
}

std::optional<DetectionsRecord> decode_detections(const std::byte* p, size_t n) {
  return run<DetectionsRecord>(p, n, [](DetectionsRecord& m, const Field& f) {
    if (f.num == 1 && is_v(f)) m.t_ns = int64_t(f.v);
    else if (f.num == 2 && is_v(f)) m.frame_id = uint32_t(f.v);
    else if (f.num == 3 && is_l(f)) {
      DetectionBox b;
      Dec d(f.p, f.n); Field g;
      while (d.next(g)) {
        if (g.num == 1 && is_v(g)) b.cls = int32_t(int64_t(g.v));
        else if (g.num >= 2 && g.num <= 6 && g.wire == 5) {
          float* dst[] = {&b.score, &b.x0, &b.y0, &b.x1, &b.y1};
          *dst[g.num - 2] = as_flt(uint32_t(g.v));
        }
      }
      if (!d.ok()) return false;
      m.boxes.push_back(b);
    }
    return true;
  });
}

std::optional<LabelsRecord> decode_labels(const std::byte* p, size_t n) {
  return run<LabelsRecord>(p, n, [](LabelsRecord& m, const Field& f) {
    if (f.num == 1 && is_v(f)) m.t_ns = int64_t(f.v);
    else if (f.num == 2 && is_l(f)) {
      LabelObject o;
      Dec d(f.p, f.n); Field g;
      while (d.next(g)) {
        if (g.num == 1 && is_v(g)) o.id = uint32_t(g.v);
        else if (g.num == 2 && is_v(g)) o.cls = int32_t(int64_t(g.v));
        else if (g.num >= 3 && g.num <= 5 && is_d(g)) o.centre[g.num - 3] = as_dbl(g);
        else if (g.num >= 6 && g.num <= 8 && is_d(g)) o.size[g.num - 6] = as_dbl(g);
        else if (g.num == 9 && is_d(g)) o.yaw = as_dbl(g);
        else if (g.num == 10 && is_d(g)) o.conf = as_dbl(g);
        else if (g.num == 11 && is_v(g)) o.n_obs = uint32_t(g.v);
      }
      if (!d.ok()) return false;
      m.objects.push_back(o);
    } else if (f.num == 3 && is_l(f)) {
      LabelOpening o;
      Dec d(f.p, f.n); Field g;
      while (d.next(g)) {
        if (g.num == 1 && is_v(g)) o.kind = int32_t(int64_t(g.v));
        else if (g.num == 2 && is_l(g)) o.wall = as_str(g);
        else if (g.num >= 3 && g.num <= 9 && is_d(g)) {
          double* dst[] = {&o.x0, &o.z0, &o.x1, &o.z1, &o.y0, &o.y1, &o.conf};
          *dst[g.num - 3] = as_dbl(g);
        }
      }
      if (!d.ok()) return false;
      m.openings.push_back(std::move(o));
    }
    return true;
  });
}

std::optional<ShellRecord> decode_shell(const std::byte* p, size_t n) {
  return run<ShellRecord>(p, n, [](ShellRecord& m, const Field& f) {
    if (f.num == 1 && is_v(f)) m.t_ns = int64_t(f.v);
    else if (f.num == 2 && is_l(f)) {
      ShellWallItem w;
      Dec d(f.p, f.n); Field g;
      while (d.next(g)) {
        if (g.num == 1 && is_l(g)) w.id = as_str(g);
        else if (g.num >= 2 && g.num <= 5 && is_d(g)) {
          double* dst[] = {&w.x0, &w.z0, &w.x1, &w.z1};
          *dst[g.num - 2] = as_dbl(g);
        } else if (g.num == 6 && is_v(g)) w.inferred = g.v != 0;
      }
      if (!d.ok()) return false;
      m.walls.push_back(std::move(w));
    } else if (f.num == 3) {
      if (is_l(f)) {
        if (f.n % 8 != 0) return false;  // packed doubles: a ragged payload is malformed, never truncated
        for (size_t i = 0; i + 8 <= f.n; i += 8) { Field g; std::memcpy(&g.v, f.p + i, 8); m.corners.push_back(as_dbl(g)); }
      } else if (is_d(f)) m.corners.push_back(as_dbl(f));
    }
    else if (f.num == 4 && is_v(f)) m.closed = f.v != 0;
    else if (f.num == 5 && is_v(f)) m.low_confidence = f.v != 0;
    else if (f.num == 6 && is_d(f)) m.ceiling_h = as_dbl(f);
    else if (f.num == 7 && is_v(f)) m.ceiling_inferred = f.v != 0;
    return true;
  });
}

std::optional<GapsRecord> decode_gaps(const std::byte* p, size_t n) {
  return run<GapsRecord>(p, n, [](GapsRecord& m, const Field& f) {
    if (f.num == 1 && is_v(f)) m.t_ns = int64_t(f.v);
    else if (f.num == 2 && is_l(f)) {
      GapItem o;
      Dec d(f.p, f.n); Field g;
      while (d.next(g)) {
        if (g.num == 1 && is_v(g)) o.type = int32_t(int64_t(g.v));
        else if (g.num >= 2 && g.num <= 6 && is_d(g)) {
          double* dst[] = {&o.x0, &o.z0, &o.x1, &o.z1, &o.pri};
          *dst[g.num - 2] = as_dbl(g);
        } else if (g.num == 7 && is_l(g)) o.why = as_str(g);
      }
      if (!d.ok()) return false;
      m.gaps.push_back(std::move(o));
    }
    return true;
  });
}

}  // namespace capturekit::bundle::codec
