#include "capturekit/guide/params.hpp"

#include "capturekit/bundle/finalize.hpp"
#include "capturekit/util/json.hpp"

namespace capturekit::guide {

namespace {
template <class F>
void each(GuideParams& c, F&& f) {
#define P(x) f(#x, c.x)
  P(cell_m); P(min_views); P(min_spread_deg); P(range_min_m); P(range_max_m); P(max_incidence_deg);
  P(min_sharp_norm); P(view_margin_px); P(complete_threshold); P(use_occlusion);
  P(nav_cell_m); P(body_radius_m); P(obstacle_min_h); P(obstacle_max_h); P(obstacle_hits);
  P(traverse_radius_m); P(camera_height_default);
  P(lattice_m); P(headings); P(pitch_up_deg); P(replan_hz); P(hysteresis); P(reach_m); P(reach_yaw_deg); P(dwell_s); P(realized_gain);
  P(min_gain); P(overlap_min); P(instr_overhead_m); P(turn_cost_m_per_rad); P(safety_margin_m); P(safety_penalty_m);
  P(cell_stride); P(revisit_decay); P(sweep_after_s); P(discover_after); P(discover_tries); P(discover_pitch_deg);
  P(min_gap_s); P(cd_slow_down); P(cd_hold_still); P(cd_move); P(cd_rotate); P(cd_tilt); P(cd_step_back);
  P(cd_finish); P(cd_light); P(cd_sweep); P(expire_s); P(blur_frames); P(lost_relocalize_s); P(tilt_deg);
  P(rotate_deg);
#undef P
}
}  // namespace

std::optional<GuideParams> GuideParams::from_json(std::string_view text, std::string* err) {
  auto obj = json::parse_flat(text.empty() ? "{}" : text, err);
  if (!obj) return std::nullopt;
  GuideParams c;
  for (auto& [k, v] : *obj) {
    bool found = false;
    if (k == "understanding") {
      if (auto* d = std::get_if<double>(&v)) c.understanding = *d;
      continue;
    }
    each(c, [&](const char* name, double& field) {
      if (k == name) {
        found = true;
        if (auto* d = std::get_if<double>(&v)) field = *d;
      }
    });
    if (!found) {
      if (err) *err = "unknown guide param: " + k;
      return std::nullopt;
    }
  }
  return c;
}

std::string GuideParams::to_json() const {
  json::Object o;
  auto c = *this;
  each(c, [&](const char* name, double& field) { o.set(name, field); });
  if (understanding != 0) o.set("understanding", understanding);
  o.set("version", version);
  return o.dump();
}

std::string GuideParams::hash() const {
  auto j = to_json();
  return bundle::sha256_hex_bytes(j.data(), j.size()).substr(0, 16);
}

}  // namespace capturekit::guide
