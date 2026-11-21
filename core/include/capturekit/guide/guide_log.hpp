#pragma once
// GuideLog (Phase 8, stage 7): every instruction with issue time, time-to-compliance and the coverage
// delta it produced, plus a coverage time series -> guidance_log.json. Deterministic: no wall clock.
#include <array>
#include <map>
#include <string>
#include <vector>

#include "capturekit/guide/instruction.hpp"

namespace capturekit::guide {

class GuideLog {
 public:
  void start(int64_t t_ns) { if (t0_ < 0) t0_ = t_ns; }
  void issued(const Instruction& i, double completeness);
  void ended(const LifeEvent& e);
  void sample(int64_t t_ns, double completeness, double seen);
  void note_target_switch() { ++target_switches_; }
  void note_unsafe_target() { ++unsafe_targets_; }
  void note_plan(bool target_set) { ++plans_; plans_with_target_ += target_set; }

  int unsafe_targets() const { return unsafe_targets_; }
  size_t instructions() const { return entries_.size(); }
  /// Fraction of guidance instructions identical (type+params) to the previous guidance one within 15 s.
  double repetition_rate() const;
  /// Full log as JSON (instructions, coverage series, summary).
  std::string to_json(const std::string& params_hash) const;
  std::string summary_json() const;

 private:
  struct Entry {
    Instruction ins;
    double t_issue_s = 0, t_end_s = -1, cov_issue = 0, cov_end = -1;
    LifeState state = LifeState::Active;
  };
  double rel(int64_t t) const { return t0_ < 0 ? 0.0 : double(t - t0_) / 1e9; }
  std::vector<Entry> entries_;
  std::vector<std::array<double, 3>> series_;
  int64_t t0_ = -1;
  int target_switches_ = 0, unsafe_targets_ = 0, plans_ = 0, plans_with_target_ = 0;
};

}  // namespace capturekit::guide
