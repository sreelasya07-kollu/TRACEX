// Replay sink bindings: Python drives ordering, fault injection and image decoding; every metric
// is computed by the C++ sink itself.
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <optional>

#include "capturekit/replay/sink.hpp"

namespace nb = nanobind;
using namespace capturekit;
using namespace capturekit::replay;

namespace {
using Luma = nb::ndarray<const uint8_t, nb::ndim<2>, nb::c_contig, nb::device::cpu>;

struct PySink {
  std::unique_ptr<ReplaySink> s;
};
}  // namespace

void bind_replay(nb::module_& root) {
  auto m = root.def_submodule("replay", "Replay sinks (t_ns-ordered stream consumers)");
  m.def("sink_names", &sink_names);

  nb::class_<PySink>(m, "Sink")
      .def("__init__", [](PySink* self, const std::string& name, const std::string& cfg) {
             new (self) PySink{make_sink(name, cfg)};
           }, nb::arg("name"), nb::arg("config_json") = "{}")
      .def_prop_ro("name", [](const PySink& p) { return std::string(p.s->name()); })
      .def_prop_ro("needs_images", [](const PySink& p) { return p.s->needs_images(); })
      .def("on_intrinsics", [](PySink& p, int64_t t, uint32_t w, uint32_t h, double fx, double fy,
                               double cx, double cy, int model, std::vector<double> dist) {
             bundle::IntrinsicsSample k;
             k.t_ns = t; k.width = w; k.height = h; k.fx = fx; k.fy = fy; k.cx = cx; k.cy = cy;
             k.model = bundle::CameraModelId(model); k.distortion = std::move(dist);
             p.s->on_intrinsics(k);
           })
      .def("on_pose", [](PySink& p, int64_t t, std::vector<double> pos, std::vector<double> q, int state,
                         int reason) {
             bundle::PoseSample s;
             s.t_ns = t;
             for (int i = 0; i < 3; ++i) s.t_world_from_camera[i] = pos.at(size_t(i));
             for (int i = 0; i < 4; ++i) s.q_world_from_camera_wxyz[i] = q.at(size_t(i));
             s.tracking_state = bundle::TrackingState(state);
             s.tracking_reason = bundle::TrackingReason(reason);
             p.s->on_pose(s);
           })
      .def("on_gyro", [](PySink& p, int64_t t, double x, double y, double z) {
             bundle::ImuSample s; s.t_ns = t; s.v[0] = x; s.v[1] = y; s.v[2] = z; p.s->on_gyro(s);
           })
      .def("on_accel", [](PySink& p, int64_t t, double x, double y, double z) {
             bundle::ImuSample s; s.t_ns = t; s.v[0] = x; s.v[1] = y; s.v[2] = z; p.s->on_accel(s);
           })
      .def("on_frame", [](PySink& p, int64_t t, uint32_t id, const std::string& file, bool keyframe,
                          int64_t exposure_ns, std::optional<Luma> luma) {
             FrameView f;
             f.t_ns = t; f.frame_id = id; f.file = file; f.keyframe = keyframe; f.exposure_ns = exposure_ns;
             if (luma) {
               f.luma = luma->data();
               f.height = int(luma->shape(0));
               f.width = int(luma->shape(1));
               f.stride = f.width;
             }
             p.s->on_frame(f);
           }, nb::arg("t_ns"), nb::arg("frame_id"), nb::arg("file"), nb::arg("keyframe"),
           nb::arg("exposure_ns"), nb::arg("luma").none() = nb::none())
      .def("on_event", [](PySink& p, int64_t t, std::string src, std::string code, std::string msg,
                          std::vector<std::pair<std::string, std::string>> attrs) {
             bundle::EventRecord e;
             e.t_ns = t; e.source = std::move(src); e.code = std::move(code); e.message = std::move(msg);
             e.attrs = std::move(attrs);
             p.s->on_event(e);
           })
      .def("on_points", [](PySink& p, int64_t t, std::vector<float> xyz) {
             bundle::PointCloudRecord r; r.t_ns = t; r.xyz = std::move(xyz); p.s->on_points(r);
           })
      .def("on_plane", [](PySink& p, int64_t t, std::string id, std::string kind, std::vector<double> c,
                          std::vector<double> n, std::vector<double> u, double eu, double ev) {
             bundle::PlaneRecord r;
             r.t_ns = t; r.id = std::move(id); r.kind = std::move(kind);
             for (int i = 0; i < 3; ++i) {
               r.center[i] = c.at(size_t(i)); r.normal[i] = n.at(size_t(i)); r.axis_u[i] = u.at(size_t(i));
             }
             r.extent_u = eu; r.extent_v = ev;
             p.s->on_plane(r);
           })
      .def("on_detections", [](PySink& p, int64_t t, uint32_t frame_id,
                               const std::vector<std::tuple<int, float, float, float, float, float>>& boxes) {
             bundle::DetectionsRecord r; r.t_ns = t; r.frame_id = frame_id;
             for (auto& [c, s, x0, y0, x1, y1] : boxes) r.boxes.push_back({c, s, x0, y0, x1, y1});
             p.s->on_detections(r);
           })
      .def("on_end", [](PySink& p, int64_t t) { p.s->on_end(t); })
      .def("report_json", [](const PySink& p) { return p.s->report_json(); });
}
