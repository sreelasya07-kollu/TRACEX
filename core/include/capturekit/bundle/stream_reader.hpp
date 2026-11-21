#pragma once
// Reads streams.mcap, finalized or not. Unfinalized files (crash, kill) are scanned
// sequentially; every message up to the first damaged record is recovered.
#include <string>
#include <vector>

#include "capturekit/bundle/types.hpp"

namespace capturekit::bundle {

struct StreamContents {
  std::vector<PoseSample> poses;
  std::vector<ImuSample> gyro, accel;
  std::vector<IntrinsicsSample> intrinsics;
  std::vector<FrameRecord> frames;
  std::vector<EventRecord> events;
  std::vector<PointCloudRecord> points;
  std::vector<PlaneRecord> planes;
  std::vector<DetectionsRecord> detections;  // schema 1.1.0; empty for older bundles
  std::vector<LabelsRecord> labels;          // schema 1.3.0 /guide/labels, /guide/shell, /guide/gaps (ADR 0028)
  std::vector<ShellRecord> shell;
  std::vector<GapsRecord> gaps;
  bool opened = false;
  bool finalized = false;  // footer + summary present and parsed
  uint64_t undecodable = 0;
  std::vector<std::string> problems;
};

StreamContents read_streams(const std::string& path);

}  // namespace capturekit::bundle
