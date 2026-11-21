#pragma once
// Streaming MCAP writer for streams.mcap: chunked, flushed on a t_ns cadence (never the
// system clock), finalized with a summary section on close().
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "capturekit/bundle/types.hpp"

namespace capturekit::bundle {

struct StreamWriterOptions {
  uint64_t chunk_size = 256 * 1024;
  int64_t flush_interval_ns = 1'000'000'000;  // close chunk + fflush at most this far apart (event time)
};

struct StreamCounts {
  std::map<std::string, uint64_t> per_topic;  // messages written
  uint64_t keyframes = 0;
  uint64_t rejected_non_monotonic = 0;
  int64_t t_start_ns = 0, t_end_ns = 0;
  uint64_t get(const std::string& topic) const {
    auto it = per_topic.find(topic);
    return it == per_topic.end() ? 0 : it->second;
  }
};

class StreamWriter {
 public:
  StreamWriter();
  ~StreamWriter();
  StreamWriter(const StreamWriter&) = delete;
  StreamWriter& operator=(const StreamWriter&) = delete;

  /// Opens (truncates) path and writes the header, schemas and channels.
  bool open(const std::string& path, const StreamWriterOptions& opts = {});
  bool is_open() const;

  // Each write returns false (and counts it) if t_ns is not strictly greater than the
  // previous message on the same topic; the sample is dropped, never reordered.
  bool write(const PoseSample& m);
  bool write_gyro(const ImuSample& m);
  bool write_accel(const ImuSample& m);
  bool write(const IntrinsicsSample& m);
  bool write(const FrameRecord& m);
  bool write(const EventRecord& m);
  bool write(const PointCloudRecord& m);
  bool write(const PlaneRecord& m);
  bool write(const DetectionsRecord& m);  // /ar/detections, schema 1.1.0
  bool write(const LabelsRecord& m);      // /guide/labels, schema 1.3.0 (derived output, ADR 0028)
  bool write(const ShellRecord& m);       // /guide/shell
  bool write(const GapsRecord& m);        // /guide/gaps

  /// Forces the current chunk to disk (used on pause / checkpoint).
  void flush();
  /// Writes the summary + footer. Safe to call twice.
  StreamCounts close();
  const StreamCounts& counts() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace capturekit::bundle
