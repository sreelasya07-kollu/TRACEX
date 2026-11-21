// Sink registry: one place that maps names to core sinks (used by ckit replay and tests).
#include <functional>
#include <map>
#include <stdexcept>

#include "capturekit/governor/governor_sink.hpp"
#include "capturekit/guide/guide_sink.hpp"
#include "capturekit/quality/quality_sink.hpp"
#include "capturekit/replay/metrics_sink.hpp"
#include "capturekit/replay/sink.hpp"

namespace capturekit::replay {

namespace {
using Factory = std::function<std::unique_ptr<ReplaySink>(const std::string&)>;
const std::map<std::string, Factory>& registry() {
  static const std::map<std::string, Factory> r = {
      {"metrics", [](const std::string&) { return std::make_unique<MetricsSink>(); }},
      {"quality", [](const std::string& c) { return quality::QualitySink::create(c); }},
      {"governor", [](const std::string& c) { return governor::GovernorSink::create(c); }},
      {"guide", [](const std::string& c) { return guide::GuideSink::create(c); }},
  };
  return r;
}
}  // namespace

std::unique_ptr<ReplaySink> make_sink(const std::string& name, const std::string& cfg) {
  auto it = registry().find(name);
  if (it == registry().end()) throw std::invalid_argument("unknown sink: " + name);
  return it->second(cfg);
}

std::vector<std::string> sink_names() {
  std::vector<std::string> n;
  for (auto& [k, _] : registry()) n.push_back(k);
  return n;
}

}  // namespace capturekit::replay
