#pragma once
// Bundle finalization helpers: camera.json, sha256 file list, manifest.json written last
// (temp file + rename so a crash never leaves a half-written manifest).
#include <cstdint>
#include <string>
#include <vector>

#include "capturekit/bundle/stream_writer.hpp"
#include "capturekit/bundle/types.hpp"

namespace capturekit::bundle {

struct FileEntry {
  std::string path;  // bundle-relative, '/' separators
  std::string sha256;
  uint64_t size = 0;
  std::string role;
};

struct BundleMeta {
  std::string schema_version = "1.1.0";
  std::string bundle_id, session_id;
  std::string platform = "other", model, os, app_version = "0.0.1";
  std::string clock_base = "unknown";
  int64_t clock_offset_ns = 0;
  std::string completion_status = "complete";  // complete | partial | recovered
  std::string completion_reason;
  std::string capture_settings_json = "{}";  // raw JSON object text
  std::string validation_json;                // optional raw JSON object text
  int display_rotation_deg = 0;
  // Schema 1.2.0 (ADR 0020), shell-supplied: the core never reads a clock. consent_at is an ISO 8601 UTC string.
  // Setting consent_version or redaction_mode != "none" makes write_manifest stamp schema_version 1.2.0.
  std::string consent_version, consent_at;
  std::string redaction_mode = "none";  // none | faces
  double q_camera_from_imu_wxyz[4] = {1, 0, 0, 0};
  // Schema 1.4.0 (ADR 0034): false = the IMU was NOT recorded (the live session's default), which makes write_manifest
  // stamp at least 1.4.0. Writers outside the live session (import-raw, golden bundles) keep the default: they write
  // what they were given.
  bool imu_recorded = true;
};

std::string sha256_hex(const std::string& file_path);
std::string sha256_hex_bytes(const void* data, size_t n);

/// Writes camera.json for the nominal (first) intrinsics.
bool write_camera_json(const std::string& bundle_dir, const IntrinsicsSample& k, const BundleMeta& meta);

/// Hashes every file in bundle_dir (except manifest.json) and returns the sorted list.
std::vector<FileEntry> list_files(const std::string& bundle_dir);

/// Writes manifest.json atomically. Counts come from the stream writer (or a reader recount).
bool write_manifest(const std::string& bundle_dir, const BundleMeta& meta, const StreamCounts& counts,
                    const std::vector<FileEntry>& files);

}  // namespace capturekit::bundle
