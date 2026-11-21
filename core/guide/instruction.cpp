#include "capturekit/guide/instruction.hpp"

#include <algorithm>
#include <cmath>

namespace capturekit::guide {

const char* type_name(InstrType t) {
  static const char* n[] = {"SLOW_DOWN", "HOLD_STILL", "RELOCALIZE", "MOVE_TO", "ROTATE_TOWARD", "TILT_UP",
                            "TILT_DOWN", "STEP_BACK", "SWEEP", "MORE_LIGHT", "FINISH", "PROGRESS", "KEEP_TURNING", "AIM_TEXTURED",
                            "RECALIBRATE"};
  return n[int(t)];
}
int type_priority(InstrType t) {
  switch (t) {
    case InstrType::SlowDown: case InstrType::HoldStill: case InstrType::Relocalize: case InstrType::AimTextured:
    case InstrType::Recalibrate: return 3;
    case InstrType::MoreLight: case InstrType::Progress: case InstrType::KeepTurning: return 1;
    default: return 2;
  }
}
const char* life_name(LifeState s) {
  static const char* n[] = {"ACTIVE", "COMPLETED", "EXPIRED", "SUPERSEDED"};
  return n[int(s)];
}

namespace {
struct Entry { std::vector<const char*> shorts, longs; };
// Message catalog (locale "en"). Placeholders {dir} {m} {m_unit} {deg}. IDs are stable keys for translation.
// Each id has 3 short phrasings (every one <= 8 words); the generator picks one deterministically from the
// instruction id so consecutive repeats do not sound identical. {m_unit} is "meter"/"meters" (see render()).
const std::map<std::string, Entry>& catalog() {
  static const std::map<std::string, Entry> c = {
      {"slow_down", {{"Slow down.", "Easy, move slower.", "A bit slower, please."},
                     {"You are moving too fast. Slow down so the photos stay sharp."}}},
      {"hold_still", {{"Hold still.", "Stop and hold steady.", "Keep the phone still."},
                      {"Tracking was lost. Hold the phone still and point it at the area you just scanned."}}},
      {"relocalize", {{"Go back to where you scanned.", "Return to an area you scanned.", "Walk back to scanned ground."},
                      {"Tracking is still lost. Walk back to an area you already scanned and move slowly."}}},
      {"move.to", {{"Walk {dir}, {m} {m_unit}.", "Go {dir}, about {m} {m_unit}.", "Head {dir}, {m} {m_unit}."},
                   {"Walk {dir} about {m} {m_unit} to reach the next area to scan."}}},
      {"rotate.to", {{"Turn {dir} toward the unscanned area.", "Face {dir} to scan more.", "Turn {dir} to the unscanned part."},
                     {"Turn {dir}, about {deg} degrees, to face the area that still needs scanning."}}},
      // Gap steering (understanding >= 2): the same move / turn with the landmark of the gap ({why}, <= 8 words itself).
      // make() falls back to move.to / rotate.to when a rendering exceeds 8 words.
      {"gap.move", {{"Walk {dir} to see {why}.", "Go {dir}, {m} {m_unit}, to see {why}.", "Head {dir} toward {why}."},
                    {"Walk {dir} about {m} {m_unit} to see {why}, which is not scanned yet."}}},
      {"gap.turn", {{"Turn {dir} to see {why}.", "Face {dir} toward {why}.", "Turn {dir}: {why} is unscanned."},
                    {"Turn {dir}, about {deg} degrees, to see {why}, which is not scanned yet."}}},
      {"tilt.up", {{"Tilt up to scan the ceiling.", "Aim up at the ceiling.", "Point the phone up."},
                   {"Tilt the phone up about {deg} degrees to scan the upper wall and ceiling."}}},
      {"tilt.down", {{"Tilt down to scan the floor.", "Aim down at the floor.", "Point the phone down."},
                     {"Tilt the phone down about {deg} degrees to scan the lower wall and floor."}}},
      {"step.back", {{"Step back to see the whole wall.", "Step back a little.", "Back up so the wall fits."},
                     {"You are too close. Step back about {m} {m_unit} so the wall fits in view."}}},
      {"sweep", {{"Turn slowly left to scan the room.", "Slowly turn left around the room.", "Turn left, nice and slow."},
                 {"Stand still and turn slowly all the way around so the phone can see the room."}}},
      {"more_light", {{"It is too dark. Turn on a light.", "Too dark. Add some light.", "Turn on a light, please."},
                      {"The image is too dark. Turn on a light or open a curtain."}}},
      {"finish", {{"Scan complete. Say done or tap finish.", "All covered. Say done or tap finish.", "Room done. Say done or tap finish."},
                  {"The room is covered. Say done or tap Finish to save the scan."}}},
      {"progress.25", {{"A quarter of the room is done.", "Quarter done. Keep going.", "Nice, a quarter is scanned."},
                       {"About a quarter of the room is scanned. Keep going."}}},
      {"progress.50", {{"Halfway there.", "Half the room is scanned.", "Halfway done. Keep going."},
                       {"About half of the room is scanned. Keep going."}}},
      {"progress.75", {{"Almost done.", "Three quarters done. Nearly there.", "Nearly there. Keep scanning."},
                       {"About three quarters of the room is scanned. Nearly there."}}},
      {"keep_turning", {{"Keep turning.", "Keep turning slowly.", "Good. Keep turning left."},
                        {"Keep turning slowly so the phone can see the whole room."}}},
      {"aim_texture", {{"Aim at a textured wall.", "Point at a wall with detail.", "Aim at something with texture."},
                       {"The phone cannot see enough detail. Aim at a textured wall or furniture."}}},
      {"recalibrate", {{"Turn your phone slowly in a small circle.", "Slowly circle the phone to recalibrate.", "Keep circling the phone slowly."},
                       {"Tracking lost. Slowly turn your phone in a small circle to recalibrate."}}},
  };
  return c;
}
// Tracking reason (bundle::TrackingReason codes 2 / 3 / 4) while tracking is LIMITED.
bool excessive_motion(const Situation& s) { return s.tracking_state == 2 && s.tracking_reason == 2; }
bool few_features(const Situation& s) { return s.tracking_state == 2 && s.tracking_reason == 3; }
bool low_light(const Situation& s) { return s.tracking_state == 2 && s.tracking_reason == 4; }
std::string dir_word(double rel_deg) {
  double a = std::fabs(rel_deg);
  if (a < 20) return "ahead";
  if (a > 150) return "around";
  std::string side = rel_deg > 0 ? "left" : "right";
  if (a < 45) return "slightly " + side;
  return side;
}
int word_count(const std::string& t) {
  int n = 0;
  bool in = false;
  for (char c : t) {
    const bool sp = c == ' ' || c == '\t';
    if (!sp && !in) ++n;
    in = !sp;
  }
  return n;
}
std::string num(double v) {
  char b[16];
  std::snprintf(b, sizeof b, v == std::floor(v) ? "%.0f" : "%.1f", v);
  return b;
}
}  // namespace

int InstructionGenerator::variant_count(const std::string& id) {
  auto it = catalog().find(id);
  return it == catalog().end() ? 0 : int(it->second.shorts.size());
}

std::string InstructionGenerator::render(const std::string& id, const std::map<std::string, std::string>& vars, bool longer,
                                         uint32_t variant) {
  auto it = catalog().find(id);
  if (it == catalog().end()) return id;
  const auto& v = longer ? it->second.longs : it->second.shorts;
  std::string s = v[variant % v.size()];
  auto fill = [&](const std::string& k, const std::string& val) {
    for (size_t p; (p = s.find("{" + k + "}")) != std::string::npos;) s.replace(p, k.size() + 2, val);
  };
  for (auto& [k, val] : vars) fill(k, val);
  // Plural-aware unit: exactly "1" is singular ("1 meter"); 0.5, 1.5, 2 ... are plural.
  if (!vars.count("m_unit")) {
    auto m = vars.find("m");
    fill("m_unit", m != vars.end() && m->second == "1" ? "meter" : "meters");
  }
  return s;
}

bool InstructionGenerator::cooled(InstrType t, int64_t now) const {
  auto it = last_issue_.find(int(t));
  if (it == last_issue_.end()) return true;
  double cd = 0;
  switch (t) {
    case InstrType::SlowDown: cd = p_.cd_slow_down; break;
    case InstrType::HoldStill: case InstrType::Relocalize: cd = p_.cd_hold_still; break;
    case InstrType::MoveTo: cd = p_.cd_move; break;
    case InstrType::RotateToward: cd = p_.cd_rotate; break;
    case InstrType::TiltUp: case InstrType::TiltDown: cd = p_.cd_tilt; break;
    case InstrType::StepBack: cd = p_.cd_step_back; break;
    case InstrType::Sweep: cd = p_.cd_sweep; break;
    case InstrType::MoreLight: cd = p_.cd_light; break;
    case InstrType::Finish: cd = p_.cd_finish; break;
    case InstrType::Progress: case InstrType::KeepTurning: cd = 0; break;  // paced by last_info_ns_
    case InstrType::AimTextured: case InstrType::Recalibrate: cd = p_.cd_hold_still; break;
  }
  return double(now - it->second) / 1e9 >= cd;
}

int InstructionGenerator::bucket(const Instruction& i) {
  switch (i.type) {
    case InstrType::MoveTo: return int(std::lround(i.param("distance_m") / 0.5)) * 100 + int(std::lround(i.param("rel_bearing_deg") / 30));
    case InstrType::RotateToward: return int(std::lround(i.param("rel_deg") / 30));
    default: return 0;
  }
}

Instruction InstructionGenerator::make(InstrType t, const Situation& s, std::vector<std::pair<std::string, double>> params) {
  Instruction i;
  i.id = next_id_++;
  i.type = t;
  i.params = std::move(params);
  i.priority = type_priority(t);
  i.t_ns = s.t_ns;
  std::map<std::string, std::string> v;
  switch (t) {
    case InstrType::SlowDown: i.msg_id = "slow_down"; i.haptic = "double"; break;
    case InstrType::HoldStill: i.msg_id = "hold_still"; i.haptic = "double"; break;
    case InstrType::Relocalize: i.msg_id = "relocalize"; i.haptic = "double"; break;
    case InstrType::MoveTo: {
      double b = i.param("rel_bearing_deg");
      i.msg_id = "move.to";
      v = {{"dir", dir_word(b)}, {"m", num(i.param("distance_m"))}};
      i.haptic = "tap";
      i.overlay_hint = "arrow:" + num(std::round(b)) + ":" + num(i.param("distance_m"));
      break;
    }
    case InstrType::RotateToward: {
      double d = i.param("rel_deg");
      i.msg_id = "rotate.to";
      v = {{"dir", dir_word(d)}, {"deg", num(std::round(std::fabs(d) / 5) * 5)}};
      i.haptic = "tap";
      i.overlay_hint = "arrow:" + num(std::round(d)) + ":0";
      break;
    }
    case InstrType::TiltUp: i.msg_id = "tilt.up"; v = {{"deg", num(std::round(i.param("deg")))}}; i.haptic = "tap"; i.overlay_hint = "tilt:+"; break;
    case InstrType::TiltDown: i.msg_id = "tilt.down"; v = {{"deg", num(std::round(i.param("deg")))}}; i.haptic = "tap"; i.overlay_hint = "tilt:-"; break;
    case InstrType::StepBack: i.msg_id = "step.back"; v = {{"m", num(i.param("meters", 0.5))}}; i.haptic = "tap"; break;
    case InstrType::Sweep: i.msg_id = "sweep"; i.haptic = "tap"; i.overlay_hint = "sweep:left"; break;
    case InstrType::MoreLight: i.msg_id = "more_light"; i.haptic = "none"; break;
    case InstrType::Finish: i.msg_id = "finish"; i.haptic = "success"; break;
    case InstrType::Progress: i.msg_id = "progress." + num(i.param("pct")); i.haptic = "none"; break;
    case InstrType::KeepTurning: i.msg_id = "keep_turning"; i.haptic = "none"; break;
    case InstrType::AimTextured: i.msg_id = "aim_texture"; i.haptic = "double"; break;
    case InstrType::Recalibrate: i.msg_id = "recalibrate"; i.haptic = "double"; i.overlay_hint = "recalibrate"; break;
  }
  const uint32_t variant = i.id - 1;  // deterministic phrasing: the first instruction always uses phrasing 0
  if ((t == InstrType::MoveTo || t == InstrType::RotateToward) && s.plan.target.gap && !s.plan.target.why.empty()) {
    const char* gid = t == InstrType::MoveTo ? "gap.move" : "gap.turn";
    auto gv = v;
    gv["why"] = s.plan.target.why;
    if (word_count(render(gid, gv, false, variant)) <= 8) i.msg_id = gid, v = gv;  // else keep the plain phrase
  }
  i.speak_text = render(i.msg_id, v, false, variant);
  i.long_text = render(i.msg_id, v, true, variant);
  return i;
}

bool InstructionGenerator::complete(Active& a, const Situation& s) const {
  const auto& u = s.user;
  switch (a.ins.type) {
    case InstrType::RotateToward: return std::fabs(wrap_pi(a.yaw0 + deg2rad(a.ins.param("rel_deg")) - u.yaw)) < deg2rad(20);
    case InstrType::MoveTo: {
      double moved = std::hypot(u.pos.x() - a.pos0.x(), u.pos.z() - a.pos0.z());
      bool at = s.plan.target.valid && std::hypot(s.plan.target.x - u.pos.x(), s.plan.target.z - u.pos.z()) < p_.reach_m;
      return moved >= 0.8 * a.ins.param("distance_m") || at;
    }
    case InstrType::TiltUp: return u.pitch >= a.pitch0 + deg2rad(a.ins.param("deg")) - deg2rad(10) || u.pitch > deg2rad(20);
    case InstrType::TiltDown: return u.pitch <= a.pitch0 - deg2rad(a.ins.param("deg")) + deg2rad(10) || u.pitch < -deg2rad(20);
    case InstrType::StepBack: return s.too_close < 0.3 || std::hypot(u.pos.x() - a.pos0.x(), u.pos.z() - a.pos0.z()) >= 0.4;
    case InstrType::Sweep: return std::fabs(a.cum_yaw) >= deg2rad(300);
    case InstrType::SlowDown: return u.speed < 0.8 && std::fabs(u.yaw_rate) < 1.0 && s.blur_streak == 0 && !excessive_motion(s);
    case InstrType::HoldStill: case InstrType::Relocalize: return s.tracking_state == 1;
    case InstrType::MoreLight: return !s.underexposed && !low_light(s);
    case InstrType::AimTextured: return !few_features(s);
    case InstrType::Recalibrate: return !s.recovering;
    case InstrType::Finish: return false;
    case InstrType::Progress: case InstrType::KeepTurning: return true;  // one-shot, spoken once
  }
  return false;
}

void InstructionGenerator::finish_active(Active& a, LifeState st, const Situation& s, std::vector<LifeEvent>& ev) {
  ev.push_back({a.ins, st, s.t_ns, s.completeness});
}

void InstructionGenerator::update(const Situation& s, std::vector<Instruction>& out, std::vector<LifeEvent>& events) {
  if (start_ns_ < 0) start_ns_ = s.t_ns;
  // 1. Lifecycle of active instructions.
  for (auto& a : active_) {
    a.cum_yaw += wrap_pi(s.user.yaw - a.last_yaw);
    a.last_yaw = s.user.yaw;
  }
  for (size_t k = 0; k < active_.size();) {
    Active& a = active_[k];
    LifeState st = LifeState::Active;
    if (complete(a, s)) st = LifeState::Completed;
    else if (double(s.t_ns - a.ins.t_ns) / 1e9 > p_.expire_s) st = LifeState::Expired;
    if (st != LifeState::Active) {
      finish_active(a, st, s, events);
      active_.erase(active_.begin() + long(k));
    } else {
      ++k;
    }
  }
  auto has_active = [&](int prio) {
    return std::any_of(active_.begin(), active_.end(), [&](auto& a) { return a.ins.priority == prio; });
  };
  auto issue = [&](Instruction ins) {
    // A new instruction supersedes an active one of the same priority class.
    for (size_t k = 0; k < active_.size();)
      if (active_[k].ins.priority == ins.priority) {
        finish_active(active_[k], LifeState::Superseded, s, events);
        active_.erase(active_.begin() + long(k));
      } else {
        ++k;
      }
    Active a;
    a.ins = ins;
    a.yaw0 = a.last_yaw = s.user.yaw;
    a.pitch0 = s.user.pitch;
    a.pos0 = s.user.pos;
    active_.push_back(a);
    last_issue_[int(ins.type)] = s.t_ns;
    if (ins.priority <= 2) last_guidance_ns_ = s.t_ns;
    out.push_back(std::move(ins));
  };
  auto same_active = [&](const Instruction& cand) {
    for (auto& a : active_)
      if (a.ins.type == cand.type && bucket(a.ins) == bucket(cand)) return true;
    return false;
  };

  auto is_active = [&](InstrType t) {
    return std::any_of(active_.begin(), active_.end(), [t](auto& a) { return a.ins.type == t; });
  };

  // 1b. Tracking recovery (ADR 0031): the pose is unreliable, so every guidance / info instruction still active is
  // superseded (its baselines would complete or expire it on drifted poses) and nothing but the recovery prompt is
  // issued. While tracking is degraded: RECALIBRATE, escalated to RELOCALIZE after a long NOT_AVAILABLE loss. While
  // tracking is back but still settling: silence (the prompt completes when recovery ends).
  if (s.recovering) {
    for (size_t k = 0; k < active_.size();)
      if (active_[k].ins.priority < 3) {
        finish_active(active_[k], LifeState::Superseded, s, events);
        active_.erase(active_.begin() + long(k));
      } else {
        ++k;
      }
    if (s.tracking_state == 1) return;
    const bool long_lost = s.tracking_state == 3 && double(s.t_ns - s.lost_since_ns) / 1e9 >= p_.lost_relocalize_s;
    const InstrType t = long_lost ? InstrType::Relocalize : InstrType::Recalibrate;
    if (!is_active(t) && cooled(t, s.t_ns) && !(t == InstrType::Recalibrate && is_active(InstrType::Relocalize)))
      issue(make(t, s, {}));  // supersedes HOLD_STILL / AIM_TEXTURED (same priority class)
    return;
  }

  // 2. Urgent interrupts (bypass the global gap, respect their own cooldown).
  if (s.tracking_state == 3) {
    bool long_lost = double(s.t_ns - s.lost_since_ns) / 1e9 >= p_.lost_relocalize_s;
    InstrType t = long_lost ? InstrType::Relocalize : InstrType::HoldStill;
    // A long loss escalates HOLD_STILL -> RELOCALIZE even while HOLD_STILL is still active.
    const bool escalate = t == InstrType::Relocalize && std::any_of(active_.begin(), active_.end(), [](auto& a) { return a.ins.type == InstrType::HoldStill; });
    if (cooled(t, s.t_ns) && (!has_active(3) || escalate)) issue(make(t, s, {}));
    return;  // nothing else while tracking is lost
  }
  // Do not scold a turn the user is making because we asked for it (unless it is wild).
  const bool turning_on_request = std::any_of(active_.begin(), active_.end(), [](auto& a) {
    return a.ins.type == InstrType::RotateToward || a.ins.type == InstrType::Sweep || a.ins.type == InstrType::MoveTo;
  }) && std::fabs(s.user.yaw_rate) < 2.0;
  if (few_features(s) && cooled(InstrType::AimTextured, s.t_ns) && !has_active(3)) {
    issue(make(InstrType::AimTextured, s, {}));
    return;
  }
  // The reason path (ARCore/ARKit report EXCESSIVE_MOTION) is not excused by a requested turn; the blur path is.
  if ((excessive_motion(s) || (s.blur_streak >= int(p_.blur_frames) && !turning_on_request)) && cooled(InstrType::SlowDown, s.t_ns) && !has_active(3)) {
    issue(make(InstrType::SlowDown, s, {}));
    return;
  }
  if (has_active(3)) return;  // let the safety instruction play out
  const bool gap_ok = last_guidance_ns_ < 0 || double(s.t_ns - last_guidance_ns_) / 1e9 >= p_.min_gap_s;

  // 3. Low-priority info.
  if ((s.underexposed || low_light(s)) && cooled(InstrType::MoreLight, s.t_ns) && gap_ok && !has_active(1)) {
    issue(make(InstrType::MoreLight, s, {}));
    return;
  }

  // 3b. One-shot progress prompts (25/50/75 % coverage, once each) and sweep progress ("Keep turning").
  // Priority 1, paced by last_info_ns_; never while the plan says Finish (the finish prompt covers it).
  const bool info_ok = last_info_ns_ < 0 || double(s.t_ns - last_info_ns_) / 1e9 >= kInfoGapS;
  // Suppressed only while the plan says Finish; spent thresholds stay spent, pending ones resume afterwards.
  if (s.plan.mode != Mode::Finish && progress_next_ < 3 && gap_ok && info_ok && !has_active(1)) {
    static const double kThr[3] = {0.25, 0.5, 0.75};
    int reached = -1;
    for (int k = progress_next_; k < 3; ++k)
      if (s.completeness >= kThr[k]) reached = k;
    if (reached >= 0) {
      progress_next_ = reached + 1;  // a jump over a threshold speaks only the highest one
      last_info_ns_ = s.t_ns;
      issue(make(InstrType::Progress, s, {{"pct", kThr[reached] * 100}}));
      return;
    }
  }
  if (gap_ok && info_ok && !has_active(1)) {
    for (auto& a : active_) {
      if (a.ins.type != InstrType::Sweep) continue;
      int due = std::min(2, int(std::fabs(a.cum_yaw) / deg2rad(100)));  // prompts at 100 and 200 degrees turned
      if (due > a.turn_prompts) {
        a.turn_prompts = due;
        last_info_ns_ = s.t_ns;
        issue(make(InstrType::KeepTurning, s, {}));
        return;
      }
      break;
    }
  }

  // 4. Guidance from the plan.
  if (!gap_ok) return;
  const auto& u = s.user;
  auto try_issue = [&](Instruction cand) {
    if (!cooled(cand.type, s.t_ns) || same_active(cand)) return false;
    issue(std::move(cand));
    return true;
  };
  switch (s.plan.mode) {
    case Mode::Finish:
      try_issue(make(InstrType::Finish, s, {}));
      return;
    case Mode::Sweep:
      if (double(s.t_ns - start_ns_) / 1e9 >= p_.sweep_after_s) try_issue(make(InstrType::Sweep, s, {{"dir", 1}, {"degrees", 360}}));
      return;
    case Mode::Target: break;
  }
  const Target& tg = s.plan.target;
  if (!tg.valid) return;
  double dx = tg.x - u.pos.x(), dz = tg.z - u.pos.z();
  double dist = std::hypot(dx, dz);
  double rel = wrap_pi(std::atan2(dx, dz) - u.yaw);  // + = left
  if (s.too_close > 0.5 && try_issue(make(InstrType::StepBack, s, {{"meters", 0.5}}))) return;
  if (dist > p_.reach_m) {
    if (std::fabs(rel) > deg2rad(p_.rotate_deg)) {
      try_issue(make(InstrType::RotateToward, s, {{"rel_deg", std::round(rad2deg(rel))}}));
    } else {
      double d = std::max(0.5, std::round(dist / 0.5) * 0.5);
      try_issue(make(InstrType::MoveTo, s, {{"distance_m", d}, {"rel_bearing_deg", std::round(rad2deg(rel))}}));
    }
    return;
  }
  double yaw_err = wrap_pi(tg.yaw - u.yaw);
  if (std::fabs(yaw_err) > deg2rad(p_.reach_yaw_deg)) {
    try_issue(make(InstrType::RotateToward, s, {{"rel_deg", std::round(rad2deg(yaw_err))}}));
    return;
  }
  double pitch_err = rad2deg(tg.pitch - u.pitch);
  if (pitch_err > p_.tilt_deg) try_issue(make(InstrType::TiltUp, s, {{"deg", std::round(pitch_err)}}));
  else if (pitch_err < -p_.tilt_deg) try_issue(make(InstrType::TiltDown, s, {{"deg", std::round(-pitch_err)}}));
}

}  // namespace capturekit::guide
