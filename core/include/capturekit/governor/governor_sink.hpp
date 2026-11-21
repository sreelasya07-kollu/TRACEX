#pragma once
// GovernorSink ("governor"): drives the Governor from recorded/injected platform events.
// Events consumed (source "platform"):
//   thermal  attrs: state = nominal|fair|serious|critical
//   power    attrs: battery_pct, charging (0/1), low_power (0/1)
//   perf     attrs: frame_time_ms, dropped_rate
// Frames and poses advance time so de-escalation dwell elapses during quiet periods.
#include "capturekit/governor/governor.hpp"
#include "capturekit/replay/sink.hpp"

#include <map>
#include <memory>

namespace capturekit::governor {

class GovernorSink : public replay::ReplaySink {
 public:
  explicit GovernorSink(const GovernorConfig& cfg) : cfg_(cfg), gov_(cfg) {}
  static std::unique_ptr<replay::ReplaySink> create(const std::string& config_json);

  const char* name() const override { return "governor"; }
  void on_event(const bundle::EventRecord& e) override;
  void on_frame(const replay::FrameView& f) override { step(f.t_ns); }
  void on_end(int64_t t_ns) override;
  std::string report_json() const override;
  const std::vector<Transition>& transitions() const { return tr_; }

 private:
  void step(int64_t t);
  GovernorConfig cfg_;
  Governor gov_;
  Signals sig_;
  std::vector<Transition> tr_;
  std::map<int, int64_t> time_in_;
  int64_t t0_ = 0, t_last_ = 0;
  bool started_ = false;
  Level cur_ = Level::Nominal;
  Policy last_;
};

}  // namespace capturekit::governor
