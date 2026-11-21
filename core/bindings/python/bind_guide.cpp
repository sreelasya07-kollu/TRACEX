// GuideMind bindings: simulator + parameters. Engine logic lives in C++ only.
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "capturekit/guide/params.hpp"
#include "capturekit/guide/sim.hpp"

namespace nb = nanobind;
using namespace capturekit::guide;

void bind_guide(nb::module_& root) {
  auto m = root.def_submodule("guide", "GuideMind (coverage, planner, instructions) and simulator");
  m.def("params_json", [](const std::string& overrides) {
    std::string err;
    auto p = GuideParams::from_json(overrides, &err);
    if (!p) throw std::invalid_argument(err);
    return p->to_json();
  }, nb::arg("overrides") = "{}", "Canonical params JSON with overrides applied (raises on unknown keys)");
  m.def("params_hash", [](const std::string& overrides) {
    std::string err;
    auto p = GuideParams::from_json(overrides, &err);
    if (!p) throw std::invalid_argument(err);
    return p->hash();
  }, nb::arg("overrides") = "{}");
  m.def("simulate", [](const std::string& room, const std::string& policy, uint64_t seed, double max_s,
                       const std::string& params_json, double compliance, double speed, bool trace) {
    sim::Config c;
    c.room = room;
    c.policy = policy;
    c.seed = seed;
    c.max_s = max_s;
    c.human.compliance = compliance;
    c.human.speed = speed;
    c.trace = trace;
    std::string err;
    auto p = GuideParams::from_json(params_json, &err);
    if (!p) throw std::invalid_argument(err);
    c.guide = *p;
    return sim::simulate(c);
  }, nb::arg("room"), nb::arg("policy"), nb::arg("seed") = 0, nb::arg("max_s") = 300.0,
     nb::arg("params_json") = "{}", nb::arg("compliance") = 0.85, nb::arg("speed") = 0.9, nb::arg("trace") = false);
}
