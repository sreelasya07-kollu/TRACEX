#include "capturekit/guide/guide_log.hpp"

#include <algorithm>
#include <cmath>

#include "capturekit/util/json.hpp"

namespace capturekit::guide {

namespace {
double r3(double x) { return std::round(x * 1000) / 1000; }
std::string params_json(const Instruction& i) {
  json::Object o;
  for (auto& [k, v] : i.params) o.set(k, r3(v));
  return o.dump();
}
}  // namespace

void GuideLog::issued(const Instruction& i, double completeness) {
  Entry e;
  e.ins = i;
  e.t_issue_s = rel(i.t_ns);
  e.cov_issue = completeness;
  entries_.push_back(e);
}

void GuideLog::ended(const LifeEvent& ev) {
  for (auto& e : entries_)
    if (e.ins.id == ev.ins.id) {
      e.state = ev.state;
      e.t_end_s = rel(ev.t_end_ns);
      e.cov_end = ev.completeness_end;
      return;
    }
}

void GuideLog::sample(int64_t t, double completeness, double seen) { series_.push_back({rel(t), completeness, seen}); }

double GuideLog::repetition_rate() const {
  int g = 0, rep = 0;
  const Entry* prev = nullptr;
  for (auto& e : entries_) {
    if (e.ins.priority > 2) continue;
    ++g;
    if (prev && prev->ins.type == e.ins.type && params_json(prev->ins) == params_json(e.ins) && e.t_issue_s - prev->t_issue_s <= 15) ++rep;
    prev = &e;
  }
  return g ? double(rep) / g : 0.0;
}

std::string GuideLog::summary_json() const {
  json::Object o;
  std::map<std::string, int> by_type, by_state;
  std::vector<double> ttc;
  uint64_t guidance = 0, info = 0;
  for (auto& e : entries_) {
    ++by_type[type_name(e.ins.type)];
    // Info prompts (PROGRESS, KEEP_TURNING) are one-shot and complete at once: keep them out of the
    // guidance statistics and report them separately.
    if (e.ins.type == InstrType::Progress || e.ins.type == InstrType::KeepTurning) {
      ++info;
      continue;
    }
    ++guidance;
    ++by_state[life_name(e.state)];
    if (e.state == LifeState::Completed) ttc.push_back(e.t_end_s - e.t_issue_s);
  }
  json::Object bt, bs;
  for (auto& [k, v] : by_type) bt.set(k, v);
  for (auto& [k, v] : by_state) bs.set(k, v);
  std::sort(ttc.begin(), ttc.end());
  o.set("instructions", guidance).set("info_prompts", info).raw("by_type", bt.dump()).raw("by_state", bs.dump());
  o.set("repetition_rate", r3(repetition_rate()));
  o.set("median_time_to_compliance_s", ttc.empty() ? 0.0 : r3(ttc[ttc.size() / 2]));
  o.set("target_switches", target_switches_).set("unsafe_targets", unsafe_targets_);
  o.set("plans", plans_).set("plans_with_target", plans_with_target_);
  return o.dump();
}

std::string GuideLog::to_json(const std::string& hash) const {
  std::vector<std::string> ins, ser;
  for (auto& e : entries_) {
    json::Object o;
    o.set("id", int64_t(e.ins.id)).set("type", type_name(e.ins.type)).raw("params", params_json(e.ins))
        .set("priority", e.ins.priority).set("msg_id", e.ins.msg_id).set("speak_text", e.ins.speak_text)
        .set("haptic", e.ins.haptic).set("overlay_hint", e.ins.overlay_hint).set("t_issue_s", r3(e.t_issue_s))
        .set("state", life_name(e.state)).set("coverage_at_issue", r3(e.cov_issue));
    if (e.t_end_s >= 0) {
      o.set("t_end_s", r3(e.t_end_s)).set("time_to_compliance_s", e.state == LifeState::Completed ? r3(e.t_end_s - e.t_issue_s) : -1.0)
          .set("coverage_delta", r3(e.cov_end - e.cov_issue));
    }
    ins.push_back(o.dump());
  }
  for (auto& s : series_) ser.push_back("[" + json::num(r3(s[0])) + "," + json::num(r3(s[1])) + "," + json::num(r3(s[2])) + "]");
  json::Object o;
  o.set("schema", "capturekit.guidance_log/1").set("params_hash", hash).raw("summary", summary_json())
      .raw("instructions", json::array(ins.begin(), ins.end())).raw("coverage_series", json::array(ser.begin(), ser.end()));
  return o.dump();
}

}  // namespace capturekit::guide
