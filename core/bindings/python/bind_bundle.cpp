#include <nanobind/nanobind.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include "capturekit/bundle/finalize.hpp"
#include "capturekit/bundle/stream_reader.hpp"
#include "capturekit/bundle/stream_writer.hpp"
#include "capturekit/bundle/validate.hpp"

namespace nb = nanobind;
using namespace capturekit::bundle;

namespace {

nb::dict counts_dict(const StreamCounts& c) {
  nb::dict d;
  nb::dict per;
  for (auto& [k, v] : c.per_topic) per[k.c_str()] = v;
  d["per_topic"] = per;
  d["keyframes"] = c.keyframes;
  d["rejected_non_monotonic"] = c.rejected_non_monotonic;
  d["t_start_ns"] = c.t_start_ns;
  d["t_end_ns"] = c.t_end_ns;
  return d;
}

StreamCounts counts_from(const nb::dict& d) {
  StreamCounts c;
  for (auto [k, v] : nb::cast<nb::dict>(d["per_topic"])) c.per_topic[nb::cast<std::string>(k)] = nb::cast<uint64_t>(v);
  c.keyframes = nb::cast<uint64_t>(d["keyframes"]);
  c.t_start_ns = nb::cast<int64_t>(d["t_start_ns"]);
  c.t_end_ns = nb::cast<int64_t>(d["t_end_ns"]);
  return c;
}

template <class T>
T get(const nb::dict& d, const char* k, T def) {
  return d.contains(k) ? nb::cast<T>(d[k]) : def;
}

BundleMeta meta_from(const nb::dict& d) {
  BundleMeta m;
  m.schema_version = get<std::string>(d, "schema_version", m.schema_version);
  m.bundle_id = get<std::string>(d, "bundle_id", "");
  m.session_id = get<std::string>(d, "session_id", "");
  m.platform = get<std::string>(d, "platform", m.platform);
  m.model = get<std::string>(d, "model", "");
  m.os = get<std::string>(d, "os", "");
  m.app_version = get<std::string>(d, "app_version", m.app_version);
  m.clock_base = get<std::string>(d, "clock_base", m.clock_base);
  m.clock_offset_ns = get<int64_t>(d, "clock_offset_ns", 0);
  m.completion_status = get<std::string>(d, "completion_status", m.completion_status);
  m.completion_reason = get<std::string>(d, "completion_reason", "");
  m.capture_settings_json = get<std::string>(d, "capture_settings_json", "{}");
  m.validation_json = get<std::string>(d, "validation_json", "");
  m.display_rotation_deg = get<int>(d, "display_rotation_deg", 0);
  m.consent_version = get<std::string>(d, "consent_version", "");
  m.consent_at = get<std::string>(d, "consent_at", "");
  m.redaction_mode = get<std::string>(d, "redaction_mode", m.redaction_mode);
  if (d.contains("q_camera_from_imu")) {
    auto q = nb::cast<std::vector<double>>(d["q_camera_from_imu"]);
    for (int i = 0; i < 4 && i < int(q.size()); ++i) m.q_camera_from_imu_wxyz[i] = q[size_t(i)];
  }
  return m;
}

IntrinsicsSample intr(int64_t t, uint32_t w, uint32_t h, double fx, double fy, double cx, double cy, int model,
                      std::vector<double> dist) {
  IntrinsicsSample k;
  k.t_ns = t; k.width = w; k.height = h; k.fx = fx; k.fy = fy; k.cx = cx; k.cy = cy;
  k.model = CameraModelId(model); k.distortion = std::move(dist);
  return k;
}

}  // namespace

void bind_bundle(nb::module_& root) {
  auto m = root.def_submodule("bundle", "Scan Bundle streams, finalize helpers");

  nb::class_<StreamWriter>(m, "StreamWriter")
      .def(nb::init<>())
      .def("open", [](StreamWriter& w, const std::string& path, uint64_t chunk_size, int64_t flush_interval_ns) {
             StreamWriterOptions o; o.chunk_size = chunk_size; o.flush_interval_ns = flush_interval_ns;
             return w.open(path, o);
           }, nb::arg("path"), nb::arg("chunk_size") = 256 * 1024, nb::arg("flush_interval_ns") = 1'000'000'000)
      .def("write_pose", [](StreamWriter& w, int64_t t, std::vector<double> p, std::vector<double> q, int state, int reason) {
             PoseSample s; s.t_ns = t;
             for (int i = 0; i < 3; ++i) s.t_world_from_camera[i] = p.at(size_t(i));
             for (int i = 0; i < 4; ++i) s.q_world_from_camera_wxyz[i] = q.at(size_t(i));
             s.tracking_state = TrackingState(state); s.tracking_reason = TrackingReason(reason);
             return w.write(s);
           }, nb::arg("t_ns"), nb::arg("t_world_from_camera"), nb::arg("q_wxyz"), nb::arg("state") = 1, nb::arg("reason") = 0)
      .def("write_gyro", [](StreamWriter& w, int64_t t, double x, double y, double z) {
             ImuSample s; s.t_ns = t; s.v[0] = x; s.v[1] = y; s.v[2] = z; return w.write_gyro(s); })
      .def("write_accel", [](StreamWriter& w, int64_t t, double x, double y, double z) {
             ImuSample s; s.t_ns = t; s.v[0] = x; s.v[1] = y; s.v[2] = z; return w.write_accel(s); })
      .def("write_intrinsics", [](StreamWriter& w, int64_t t, uint32_t wd, uint32_t h, double fx, double fy, double cx,
                                  double cy, int model, std::vector<double> dist) {
             return w.write(intr(t, wd, h, fx, fy, cx, cy, model, std::move(dist)));
           }, nb::arg("t_ns"), nb::arg("width"), nb::arg("height"), nb::arg("fx"), nb::arg("fy"), nb::arg("cx"),
           nb::arg("cy"), nb::arg("model") = 0, nb::arg("distortion") = std::vector<double>{})
      .def("write_frame", [](StreamWriter& w, const nb::dict& d) {
             FrameRecord f;
             f.t_ns = nb::cast<int64_t>(d["t_ns"]);
             f.frame_id = get<uint32_t>(d, "frame_id", 0);
             f.file = get<std::string>(d, "file", "");
             f.keyframe = get<bool>(d, "keyframe", false);
             f.exposure_ns = get<int64_t>(d, "exposure_ns", 0);
             f.sharpness = get<double>(d, "sharpness", 0);
             f.sharpness_norm = get<double>(d, "sharpness_norm", 0);
             f.blur_risk_px = get<double>(d, "blur_risk_px", 0);
             f.clip_low = get<double>(d, "clip_low", 0);
             f.clip_high = get<double>(d, "clip_high", 0);
             f.corners = get<uint32_t>(d, "corners", 0);
             f.corner_spread = get<double>(d, "corner_spread", 0);
             f.quality = get<double>(d, "quality", 0);
             f.reasons = get<std::vector<std::string>>(d, "reasons", {});
             f.tracking_state = TrackingState(get<int>(d, "tracking_state", 0));
             return w.write(f);
           })
      .def("write_event", [](StreamWriter& w, int64_t t, std::string source, std::string code, std::string message,
                             std::vector<std::pair<std::string, std::string>> attrs) {
             EventRecord e; e.t_ns = t; e.source = std::move(source); e.code = std::move(code);
             e.message = std::move(message); e.attrs = std::move(attrs);
             return w.write(e);
           }, nb::arg("t_ns"), nb::arg("source"), nb::arg("code"), nb::arg("message") = "",
           nb::arg("attrs") = std::vector<std::pair<std::string, std::string>>{})
      .def("write_points", [](StreamWriter& w, int64_t t, std::vector<float> xyz) {
             PointCloudRecord p; p.t_ns = t; p.xyz = std::move(xyz); return w.write(p); })
      .def("write_plane", [](StreamWriter& w, const nb::dict& d) {
             PlaneRecord p;
             p.t_ns = nb::cast<int64_t>(d["t_ns"]);
             p.id = nb::cast<std::string>(d["id"]);
             p.kind = nb::cast<std::string>(d["kind"]);
             auto c = nb::cast<std::vector<double>>(d["center"]);
             auto n = nb::cast<std::vector<double>>(d["normal"]);
             auto u = nb::cast<std::vector<double>>(d["axis_u"]);
             for (int i = 0; i < 3; ++i) { p.center[i] = c.at(size_t(i)); p.normal[i] = n.at(size_t(i)); p.axis_u[i] = u.at(size_t(i)); }
             auto e = nb::cast<std::vector<double>>(d["extent"]);
             p.extent_u = e.at(0); p.extent_v = e.at(1);
             return w.write(p);
           })
      .def("write_detections", [](StreamWriter& w, int64_t t, uint32_t frame_id,
                                  const std::vector<std::tuple<int, float, float, float, float, float>>& boxes) {
             DetectionsRecord r; r.t_ns = t; r.frame_id = frame_id;
             for (auto& [c, s, x0, y0, x1, y1] : boxes) r.boxes.push_back({c, s, x0, y0, x1, y1});
             return w.write(r);
           }, nb::arg("t_ns"), nb::arg("frame_id"), nb::arg("boxes"))
      .def("write_labels", [](StreamWriter& w, int64_t t,
                              const std::vector<std::tuple<uint32_t, int, double, double, double, double, double, double, double, double, uint32_t>>& objs,
                              const std::vector<std::tuple<int, std::string, double, double, double, double, double, double, double>>& ops) {
             LabelsRecord r; r.t_ns = t;
             for (auto& [id, c, cx, cy, cz, sx, sy, sz, yaw, conf, n] : objs) {
               LabelObject o; o.id = id; o.cls = c; o.centre[0] = cx; o.centre[1] = cy; o.centre[2] = cz;
               o.size[0] = sx; o.size[1] = sy; o.size[2] = sz; o.yaw = yaw; o.conf = conf; o.n_obs = n;
               r.objects.push_back(o);
             }
             for (auto& [k, wall, x0, z0, x1, z1, y0, y1, conf] : ops) {
               LabelOpening o; o.kind = k; o.wall = wall; o.x0 = x0; o.z0 = z0; o.x1 = x1; o.z1 = z1; o.y0 = y0; o.y1 = y1; o.conf = conf;
               r.openings.push_back(std::move(o));
             }
             return w.write(r);
           }, nb::arg("t_ns"), nb::arg("objects"), nb::arg("openings"))
      .def("write_shell", [](StreamWriter& w, int64_t t,
                             const std::vector<std::tuple<std::string, double, double, double, double, bool>>& walls,
                             std::vector<double> corners, bool closed, bool low_conf, double ceiling_h, bool ceiling_inferred) {
             ShellRecord r; r.t_ns = t; r.corners = std::move(corners); r.closed = closed; r.low_confidence = low_conf;
             r.ceiling_h = ceiling_h; r.ceiling_inferred = ceiling_inferred;
             for (auto& [id, x0, z0, x1, z1, inf] : walls) r.walls.push_back({id, x0, z0, x1, z1, inf});
             return w.write(r);
           }, nb::arg("t_ns"), nb::arg("walls"), nb::arg("corners"), nb::arg("closed"), nb::arg("low_confidence"),
           nb::arg("ceiling_h"), nb::arg("ceiling_inferred"))
      .def("write_gaps", [](StreamWriter& w, int64_t t,
                            const std::vector<std::tuple<int, double, double, double, double, double, std::string>>& gaps) {
             GapsRecord r; r.t_ns = t;
             for (auto& [ty, x0, z0, x1, z1, pri, why] : gaps) r.gaps.push_back({ty, x0, z0, x1, z1, pri, why});
             return w.write(r);
           }, nb::arg("t_ns"), nb::arg("gaps"))
      .def("flush", &StreamWriter::flush)
      .def("close", [](StreamWriter& w) { return counts_dict(w.close()); })
      .def("counts", [](StreamWriter& w) { return counts_dict(w.counts()); });

  m.def("validate_semantic", [](const std::string& dir, const std::string& completion) {
    auto c = read_streams(dir + "/streams.mcap");
    return validate_semantic(dir, c, completion, nullptr).to_json();
  }, nb::arg("bundle_dir"), nb::arg("completion") = "complete");
  m.def("read_streams", [](const std::string& path) {
    auto r = read_streams(path);
    nb::dict d;
    d["opened"] = r.opened;
    d["finalized"] = r.finalized;
    d["undecodable"] = r.undecodable;
    d["problems"] = r.problems;
    nb::list poses;
    for (auto& p : r.poses) {
      nb::dict e;
      e["t_ns"] = p.t_ns;
      e["p"] = std::vector<double>(p.t_world_from_camera, p.t_world_from_camera + 3);
      e["q"] = std::vector<double>(p.q_world_from_camera_wxyz, p.q_world_from_camera_wxyz + 4);
      e["state"] = int(p.tracking_state);
      e["reason"] = int(p.tracking_reason);
      poses.append(e);
    }
    d["poses"] = poses;
    auto imu = [](const std::vector<ImuSample>& v) {
      nb::list l;
      for (auto& s : v) l.append(nb::make_tuple(s.t_ns, s.v[0], s.v[1], s.v[2]));
      return l;
    };
    d["gyro"] = imu(r.gyro);
    d["accel"] = imu(r.accel);
    nb::list ks;
    for (auto& k : r.intrinsics) {
      nb::dict e;
      e["t_ns"] = k.t_ns; e["width"] = k.width; e["height"] = k.height; e["fx"] = k.fx; e["fy"] = k.fy;
      e["cx"] = k.cx; e["cy"] = k.cy; e["model"] = int(k.model); e["distortion"] = k.distortion;
      ks.append(e);
    }
    d["intrinsics"] = ks;
    nb::list fr;
    for (auto& f : r.frames) {
      nb::dict e;
      e["t_ns"] = f.t_ns; e["frame_id"] = f.frame_id; e["file"] = f.file; e["keyframe"] = f.keyframe;
      e["exposure_ns"] = f.exposure_ns; e["sharpness"] = f.sharpness; e["quality"] = f.quality;
      e["reasons"] = f.reasons; e["tracking_state"] = int(f.tracking_state);
      fr.append(e);
    }
    d["frames"] = fr;
    nb::list ev;
    for (auto& e : r.events) ev.append(nb::make_tuple(e.t_ns, e.source, e.code, e.message, e.attrs));
    d["events"] = ev;
    d["n_points"] = r.points.size();
    d["n_planes"] = r.planes.size();
    nb::list pts;
    for (auto& p : r.points) pts.append(nb::make_tuple(p.t_ns, p.xyz));
    d["points"] = pts;
    nb::list pls;
    for (auto& p : r.planes) {
      nb::dict e;
      e["t_ns"] = p.t_ns; e["id"] = p.id; e["kind"] = p.kind;
      e["center"] = std::vector<double>(p.center, p.center + 3);
      e["normal"] = std::vector<double>(p.normal, p.normal + 3);
      e["axis_u"] = std::vector<double>(p.axis_u, p.axis_u + 3);
      e["extent_u"] = p.extent_u; e["extent_v"] = p.extent_v;
      pls.append(e);
    }
    d["planes"] = pls;
    nb::list dets;
    for (auto& r2 : r.detections) {
      nb::dict e;
      e["t_ns"] = r2.t_ns; e["frame_id"] = r2.frame_id;
      nb::list bx;
      for (auto& b : r2.boxes) bx.append(nb::make_tuple(b.cls, b.score, b.x0, b.y0, b.x1, b.y1));
      e["boxes"] = bx;
      dets.append(e);
    }
    d["detections"] = dets;
    nb::list lbl, shl, gps;
    for (auto& r2 : r.labels) {
      nb::dict e;
      e["t_ns"] = r2.t_ns;
      nb::list os, ops;
      for (auto& o : r2.objects)
        os.append(nb::make_tuple(o.id, o.cls, o.centre[0], o.centre[1], o.centre[2], o.size[0], o.size[1], o.size[2], o.yaw, o.conf, o.n_obs));
      for (auto& o : r2.openings) ops.append(nb::make_tuple(o.kind, o.wall, o.x0, o.z0, o.x1, o.z1, o.y0, o.y1, o.conf));
      e["objects"] = os; e["openings"] = ops;
      lbl.append(e);
    }
    for (auto& r2 : r.shell) {
      nb::dict e;
      e["t_ns"] = r2.t_ns;
      nb::list ws;
      for (auto& w : r2.walls) ws.append(nb::make_tuple(w.id, w.x0, w.z0, w.x1, w.z1, w.inferred));
      e["walls"] = ws; e["corners"] = r2.corners; e["closed"] = r2.closed; e["low_confidence"] = r2.low_confidence;
      e["ceiling_h"] = r2.ceiling_h; e["ceiling_inferred"] = r2.ceiling_inferred;
      shl.append(e);
    }
    for (auto& r2 : r.gaps) {
      nb::dict e;
      e["t_ns"] = r2.t_ns;
      nb::list gs;
      for (auto& g : r2.gaps) gs.append(nb::make_tuple(g.type, g.x0, g.z0, g.x1, g.z1, g.pri, g.why));
      e["gaps"] = gs;
      gps.append(e);
    }
    d["labels"] = lbl; d["shell"] = shl; d["gaps"] = gps;
    return d;
  });

  m.def("write_camera_json", [](const std::string& dir, uint32_t w, uint32_t h, double fx, double fy, double cx,
                                double cy, int model, std::vector<double> dist, const nb::dict& meta) {
    return write_camera_json(dir, intr(0, w, h, fx, fy, cx, cy, model, std::move(dist)), meta_from(meta));
  });
  m.def("list_files", [](const std::string& dir) {
    nb::list l;
    for (auto& f : list_files(dir)) {
      nb::dict d;
      d["path"] = f.path; d["sha256"] = f.sha256; d["size"] = f.size; d["role"] = f.role;
      l.append(d);
    }
    return l;
  });
  m.def("write_manifest", [](const std::string& dir, const nb::dict& meta, const nb::dict& counts, const nb::list& files) {
    std::vector<FileEntry> fs;
    for (auto h : files) {
      auto d = nb::cast<nb::dict>(h);
      FileEntry e;
      e.path = nb::cast<std::string>(d["path"]); e.sha256 = nb::cast<std::string>(d["sha256"]);
      e.size = nb::cast<uint64_t>(d["size"]); e.role = nb::cast<std::string>(d["role"]);
      fs.push_back(std::move(e));
    }
    return write_manifest(dir, meta_from(meta), counts_from(counts), fs);
  });
  m.def("sha256_file", &sha256_hex);
}
