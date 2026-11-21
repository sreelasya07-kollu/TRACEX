#pragma once
// On-device validator subset (Phase 7): the semantic checks of `ckit validate` that need only the
// bundle itself, with identical codes, severities and thresholds (parity test in tools/tests).
// Integrity (sha256/size) is guaranteed by finalize(); structural schema checks stay in ckit.
#include <string>
#include <vector>

#include "capturekit/bundle/stream_reader.hpp"
#include "capturekit/bundle/stream_writer.hpp"

namespace capturekit::bundle {

struct ValidationIssue {
  std::string code, severity, level, message, where;
};

struct ValidationReport {
  std::vector<ValidationIssue> issues;
  std::string verdict() const;  // FAIL if any ERROR, else WARN if any WARN, else PASS
  bool has(const std::string& code) const;
  std::string to_json() const;  // {"verdict":..,"level":"semantic","issues":[...]} (sorted keys)
};

constexpr int64_t kKeyframePoseTolNs = 50'000'000;
constexpr int64_t kPoseGapWarnNs = 500'000'000;
constexpr double kQuatTol = 1e-3;
constexpr size_t kMinKeyframes = 10;

/// completion_status: "complete" | "partial" | "recovered". counts: optional, for COUNT_MISMATCH.
ValidationReport validate_semantic(const std::string& bundle_dir, const StreamContents& s,
                                   const std::string& completion_status, const StreamCounts* counts);

}  // namespace capturekit::bundle
