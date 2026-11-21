// Geometry bindings. Poses cross the boundary as (t[3], q_wxyz[4]); intrinsics as camera.json
// dicts {width,height,fx,fy,cx,cy,model,distortion}. No bare 4x4 matrices.
#include <nanobind/nanobind.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include "capturekit/geometry/geometry.hpp"

namespace nb = nanobind;
using namespace capturekit::geometry;

namespace {

using V3 = std::array<double, 3>;
using Q4 = std::array<double, 4>;
using TQ = std::pair<V3, Q4>;

template <class P>
TQ tq(const P& p) {
  TQ o;
  p.to_tq(o.first.data(), o.second.data());
  return o;
}

template <class P>
P pose(const V3& t, const Q4& q) {
  return P::from_tq(t.data(), q.data());
}

Intrinsics K_from(const nb::dict& d) {
  Intrinsics K;
  K.width = nb::cast<uint32_t>(d["width"]);
  K.height = nb::cast<uint32_t>(d["height"]);
  K.fx = nb::cast<double>(d["fx"]);
  K.fy = nb::cast<double>(d["fy"]);
  K.cx = nb::cast<double>(d["cx"]);
  K.cy = nb::cast<double>(d["cy"]);
  std::string model = d.contains("model") ? nb::cast<std::string>(d["model"]) : "PINHOLE";
  if (model == "OPENCV") {
    K.model = CameraModel::OpenCV;
    auto dist = nb::cast<std::vector<double>>(d["distortion"]);
    if (dist.size() != 4) throw std::invalid_argument("OPENCV model needs distortion [k1,k2,p1,p2]");
    K.k1 = dist[0], K.k2 = dist[1], K.p1 = dist[2], K.p2 = dist[3];
  } else if (model != "PINHOLE") {
    throw std::invalid_argument("unknown camera model " + model);
  }
  return K;
}

nb::dict K_to(const Intrinsics& K) {
  nb::dict d;
  d["width"] = K.width;
  d["height"] = K.height;
  d["fx"] = K.fx;
  d["fy"] = K.fy;
  d["cx"] = K.cx;
  d["cy"] = K.cy;
  bool cv = K.model == CameraModel::OpenCV;
  d["model"] = cv ? "OPENCV" : "PINHOLE";
  d["distortion"] = cv ? std::vector<double>{K.k1, K.k2, K.p1, K.p2} : std::vector<double>{};
  return d;
}

PoseTrack track_from(const std::vector<int64_t>& t_ns, const std::vector<V3>& t,
                     const std::vector<Q4>& q) {
  if (t_ns.size() != t.size() || t.size() != q.size())
    throw std::invalid_argument("track arrays differ in length");
  PoseTrack tr;
  for (size_t i = 0; i < t_ns.size(); ++i) tr.push(t_ns[i], pose<PoseWC>(t[i], q[i]));
  return tr;
}

}  // namespace

void bind_geometry(nb::module_& root) {
  auto m = root.def_submodule("geometry", "Frame conventions (conventions.md); q is [w,x,y,z]");

  m.def("from_arkit", [](const V3& t, const Q4& q) {
    return tq(from_arkit(pose<PoseWorldFromGlCamera>(t, q)));
  }, nb::arg("t"), nb::arg("q_wxyz"), "ARKit world_from_camera (GL axes) -> T_world_from_camera");

  m.def("from_arcore", [](const V3& t, const Q4& q_xyzw) {
    ArcorePose a{{t[0], t[1], t[2]}, {q_xyzw[0], q_xyzw[1], q_xyzw[2], q_xyzw[3]}};
    return tq(from_arcore(a));
  }, nb::arg("t"), nb::arg("q_xyzw"), "ARCore sensor pose (q x,y,z,w) -> T_world_from_camera");

  m.def("to_colmap", [](const V3& t, const Q4& q, const nb::dict& K) {
    auto c = to_colmap(pose<PoseWC>(t, q), K_from(K));
    nb::dict d;
    d["model"] = c.model;
    d["width"] = c.width;
    d["height"] = c.height;
    d["params"] = c.params;
    d["qvec_wxyz"] = c.qvec_wxyz;
    d["tvec"] = c.tvec;
    return d;
  }, nb::arg("t"), nb::arg("q_wxyz"), nb::arg("intrinsics"));

  m.def("to_opengl", [](const V3& t, const Q4& q, const nb::dict& K, double n, double f) {
    auto g = to_opengl(pose<PoseWC>(t, q), K_from(K), n, f);
    nb::dict d;
    d["view_glcamera_from_world"] = tq(g.view);
    d["projection_col_major"] = g.projection_col_major;
    return d;
  }, nb::arg("t"), nb::arg("q_wxyz"), nb::arg("intrinsics"), nb::arg("z_near"), nb::arg("z_far"));

  m.def("rotate_image", [](const V3& t, const Q4& q, const nb::dict& K, int deg) {
    auto [T2, K2] = rotate_image(pose<PoseWC>(t, q), K_from(K), deg);
    auto o = tq(T2);
    return std::make_tuple(o.first, o.second, K_to(K2));
  }, nb::arg("t"), nb::arg("q_wxyz"), nb::arg("intrinsics"), nb::arg("deg_cw"));

  m.def("scale_intrinsics", [](const nb::dict& K, uint32_t w, uint32_t h) {
    return K_to(scale_intrinsics(K_from(K), w, h));
  });
  m.def("crop_intrinsics", [](const nb::dict& K, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    return K_to(crop_intrinsics(K_from(K), x0, y0, w, h));
  });

  m.def("project", [](const nb::dict& K, const V3& t, const Q4& q, const std::vector<V3>& pts) {
    auto Kc = K_from(K);
    auto T = pose<PoseWC>(t, q);
    std::vector<std::optional<std::array<double, 2>>> out;
    out.reserve(pts.size());
    for (auto& p : pts) {
      auto px = project(Kc, T, Eigen::Vector3d(p[0], p[1], p[2]));
      out.push_back(px ? std::optional<std::array<double, 2>>({px->x(), px->y()}) : std::nullopt);
    }
    return out;
  }, nb::arg("intrinsics"), nb::arg("t"), nb::arg("q_wxyz"), nb::arg("points_world"));

  m.def("interpolate_at", [](const std::vector<int64_t>& t_ns, const std::vector<V3>& t,
                             const std::vector<Q4>& q, const std::vector<int64_t>& query,
                             int64_t max_gap_ns) {
    auto tr = track_from(t_ns, t, q);
    std::vector<std::optional<TQ>> out;
    for (auto tq_ns : query) {
      auto p = tr.interpolate_at(tq_ns, max_gap_ns);
      out.push_back(p ? std::optional<TQ>(tq(*p)) : std::nullopt);
    }
    return out;
  }, nb::arg("t_ns"), nb::arg("t"), nb::arg("q_wxyz"), nb::arg("query_ns"), nb::arg("max_gap_ns"),
     "Interpolate a pose track at query times; None where it would extrapolate or gap too wide");

  m.def("nearest_lookup", [](const std::vector<int64_t>& t_ns, const std::vector<int64_t>& query,
                             int64_t tolerance_ns) {
    PoseTrack tr;
    for (auto t : t_ns) tr.push(t, PoseWC());
    std::vector<std::optional<size_t>> out;
    for (auto t : query) out.push_back(tr.nearest(t, tolerance_ns));
    return out;
  }, nb::arg("t_ns"), nb::arg("query_ns"), nb::arg("tolerance_ns"));
}
