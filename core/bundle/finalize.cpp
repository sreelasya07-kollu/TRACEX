#include "capturekit/bundle/finalize.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "capturekit/util/json.hpp"

namespace fs = std::filesystem;

namespace capturekit::bundle {
namespace {

bool write_atomic(const fs::path& path, const std::string& text) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << text;
    o.flush();
    if (!o) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  return !ec;
}

std::string role_for(const std::string& rel) {
  if (rel == "streams.mcap") return "streams";
  if (rel == "camera.json") return "camera";
  if (rel.rfind("keyframes/", 0) == 0) return "keyframe";
  if (rel == "guidance_log.json") return "guidance";
  if (rel.rfind("config/", 0) == 0) return "config";
  if (rel.rfind("privacy/", 0) == 0) return "redaction";
  return "other";
}

/// The schema version the content needs, never lowering a newer m.schema_version: consent or redaction metadata makes
/// the manifest a 1.2.0 one (ADR 0020), guide output channels a 1.3.0 one (ADR 0028), an unrecorded IMU a 1.4.0 one
/// (ADR 0034). The newest feature present wins.
std::string effective_schema(const BundleMeta& m, const StreamCounts& c) {
  int needed = 0;  // minor of 1.x; 0 = nothing beyond what schema_version already says
  if (!m.consent_version.empty() || m.redaction_mode != "none") needed = 2;
  if (c.get(topic::kGuideLabels) + c.get(topic::kGuideShell) + c.get(topic::kGuideGaps) > 0) needed = 3;
  if (!m.imu_recorded) needed = 4;
  if (needed == 0) return m.schema_version;
  int major = 0, minor = 0;
  if (std::sscanf(m.schema_version.c_str(), "%d.%d.", &major, &minor) == 2 && (major > 1 || minor >= needed))
    return m.schema_version;
  return "1." + std::to_string(needed) + ".0";
}

}  // namespace

bool write_camera_json(const std::string& bundle_dir, const IntrinsicsSample& k, const BundleMeta& meta) {
  using namespace capturekit::json;
  std::ostringstream o;
  o << "{\n  \"model\": " << str(k.model == CameraModelId::OpenCV ? "OPENCV" : "PINHOLE")
    << ",\n  \"width\": " << k.width << ",\n  \"height\": " << k.height
    << ",\n  \"fx\": " << num(k.fx) << ",\n  \"fy\": " << num(k.fy)
    << ",\n  \"cx\": " << num(k.cx) << ",\n  \"cy\": " << num(k.cy) << ",\n  \"distortion\": [";
  for (size_t i = 0; i < k.distortion.size(); ++i) o << (i ? ", " : "") << num(k.distortion[i]);
  o << "],\n  \"orientation\": {\"storage\": \"SENSOR_NATIVE\", \"display_rotation_deg\": "
    << meta.display_rotation_deg << "},\n  \"q_camera_from_imu\": [";
  for (int i = 0; i < 4; ++i) o << (i ? ", " : "") << num(meta.q_camera_from_imu_wxyz[i]);
  o << "]\n}\n";
  return write_atomic(fs::path(bundle_dir) / "camera.json", o.str());
}

std::vector<FileEntry> list_files(const std::string& bundle_dir) {
  std::vector<FileEntry> out;
  const fs::path root(bundle_dir);
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file()) continue;
    std::string rel = fs::relative(it->path(), root).generic_string();
    if (rel == "manifest.json" || rel.size() > 4 && rel.substr(rel.size() - 4) == ".tmp") continue;
    FileEntry e;
    e.path = rel;
    e.size = uint64_t(it->file_size());
    e.sha256 = sha256_hex(it->path().string());
    e.role = role_for(rel);
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
  return out;
}

bool write_manifest(const std::string& bundle_dir, const BundleMeta& m, const StreamCounts& c,
                    const std::vector<FileEntry>& files) {
  using namespace capturekit::json;
  std::ostringstream o;
  o << "{\n  \"schema_version\": " << str(effective_schema(m, c))
    << ",\n  \"bundle_id\": " << str(m.bundle_id) << ",\n  \"session_id\": " << str(m.session_id)
    << ",\n  \"device\": {\"platform\": " << str(m.platform) << ", \"model\": " << str(m.model)
    << ", \"os\": " << str(m.os) << ", \"app_version\": " << str(m.app_version) << "}"
    << ",\n  \"conventions\": {\"world\": \"Y_UP_GRAVITY_METRES\", \"camera\": \"OPENCV\", \"quaternion\": \"WXYZ\", "
       "\"time\": \"MONOTONIC_NS\", \"pose\": \"T_WORLD_FROM_CAMERA\"}"
    << ",\n  \"files\": [";
  for (size_t i = 0; i < files.size(); ++i) {
    const auto& f = files[i];
    o << (i ? ",\n    " : "\n    ") << "{\"path\": " << str(f.path) << ", \"sha256\": " << str(f.sha256)
      << ", \"size\": " << f.size << ", \"role\": " << str(f.role) << "}";
  }
  o << "\n  ]";
  if (!m.consent_version.empty())
    o << ",\n  \"consent\": {\"version\": " << str(m.consent_version) << ", \"at\": " << str(m.consent_at) << "}";
  if (!m.consent_version.empty() || m.redaction_mode != "none")
    o << ",\n  \"privacy\": {\"redaction_mode\": " << str(m.redaction_mode) << "}";
  o << ",\n  \"counts\": {\"poses\": " << c.get(topic::kPose) << ", \"imu_gyro\": " << c.get(topic::kGyro)
    << ", \"imu_accel\": " << c.get(topic::kAccel) << ", \"intrinsics\": " << c.get(topic::kIntrinsics)
    << ", \"frames\": " << c.get(topic::kFrameIndex) << ", \"keyframes\": " << c.keyframes
    << ", \"events\": " << c.get(topic::kEvents) << ", \"ar_points\": " << c.get(topic::kPoints)
    << ", \"ar_planes\": " << c.get(topic::kPlanes) << ", \"detections\": " << c.get(topic::kDetections)
    << (c.get(topic::kGuideLabels) + c.get(topic::kGuideShell) + c.get(topic::kGuideGaps) > 0
            ? ", \"guide_labels\": " + std::to_string(c.get(topic::kGuideLabels)) + ", \"guide_shell\": " + std::to_string(c.get(topic::kGuideShell)) +
                  ", \"guide_gaps\": " + std::to_string(c.get(topic::kGuideGaps))
            : std::string())
    << "}"
    << ",\n  \"clock\": {\"base\": " << str(m.clock_base) << ", \"offset_ns\": " << m.clock_offset_ns
    << ", \"t_start_ns\": " << c.t_start_ns << ", \"t_end_ns\": " << c.t_end_ns << "}"
    << ",\n  \"capture_settings\": " << (m.capture_settings_json.empty() ? "{}" : m.capture_settings_json)
    << ",\n  \"completion\": {\"status\": " << str(m.completion_status);
  if (!m.completion_reason.empty()) o << ", \"reason\": " << str(m.completion_reason);
  o << "}";
  if (!m.validation_json.empty()) o << ",\n  \"validation\": " << m.validation_json;
  o << "\n}\n";
  return write_atomic(fs::path(bundle_dir) / "manifest.json", o.str());
}

}  // namespace capturekit::bundle
