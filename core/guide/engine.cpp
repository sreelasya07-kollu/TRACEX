#include "capturekit/guide/engine.hpp"

#include <algorithm>
#include <cmath>

#include "capturekit/util/json.hpp"

namespace capturekit::guide {

GuideEngine::GuideEngine(const GuideParams& p)
    : p_(p), cov_(p), nav_(p), planner_(p), gen_(p), vcov_(p) {
  map_.set_classify(p.understanding != 0);
  if (steer()) planner_.set_steer(SteerParams{});
}

void GuideEngine::refresh_layout(int64_t t) {
  // At most once per event-second (t_ns, never the clock), even while plane updates keep changing the map version.
  if (last_layout_t_ >= 0 && t - last_layout_t_ < 1'000'000'000) return;
  last_layout_t_ = t;
  LayoutEvidence ev{&pts_, &nav_};
  shell_.update(map_, &wall_ids_, openings_, &ev);
}

void GuideEngine::refresh_steer(int64_t t) {
  if (!steer() || (last_steer_t_ >= 0 && t - last_steer_t_ < 1'000'000'000)) return;
  last_steer_t_ = t;
  vcov_.sync_virtual(GapFinder::virtual_surfaces(shell_.shell(), map_));
  GapInputs gi;
  gi.shell = &shell_.shell(), gi.map = &map_, gi.objects = &objects_, gi.openings = &shell_.openings();
  gi.cov = &cov_, gi.vcov = &vcov_;
  // No user pose: relative phrases ("on your left") would be stale by the time the user acts on them.
  gaps_ = gapper_.find(gi);
}

void GuideEngine::on_intrinsics(const bundle::IntrinsicsSample& k) {
  K_.width = k.width, K_.height = k.height, K_.fx = k.fx, K_.fy = k.fy, K_.cx = k.cx, K_.cy = k.cy;
  have_K_ = k.width > 0 && k.fx > 0;
}

void GuideEngine::on_pose(const bundle::PoseSample& s) {
  auto T = geometry::PoseWC::from_tq(s.t_world_from_camera, s.q_world_from_camera_wxyz);
  if (!T.translation().allFinite()) return;
  const int st = int(s.tracking_state);
  if (st == 3 && tracking_ != 3) lost_since_ = s.t_ns;
  tracking_ = st == 0 ? 1 : st;
  tracking_reason_ = tracking_ == 2 ? int(s.tracking_reason) : 0;
  if (have_pose_ && s.t_ns <= last_pose_t_) return;  // strictly increasing only
  try {
    track_.push(s.t_ns, T);
  } catch (const std::invalid_argument&) {
    return;
  }
  Vec3 pos = T.translation();
  double yaw = yaw_of(T), pitch = pitch_of(T);
  const size_t n_rec = rec_events_.size();
  rec_.on_pose(s.t_ns, st, int(s.tracking_reason), pos, yaw, rec_events_);
  bool reset = false;
  for (size_t k = n_rec; k < rec_events_.size(); ++k) reset |= rec_events_[k].kind == RecoveryEvent::Kind::Reset;
  if (have_pose_ && !reset) {
    double dt = double(s.t_ns - last_pose_t_) / 1e9;
    if (dt > 0 && dt < 1.0) {
      double a = std::min(1.0, dt / 0.3);
      double v = std::hypot(pos.x() - user_.pos.x(), pos.z() - user_.pos.z()) / dt;
      double w = wrap_pi(yaw - user_.yaw) / dt;
      user_.speed += a * (v - user_.speed);
      user_.yaw_rate += a * (w - user_.yaw_rate);
    }
  }
  user_.pos = pos, user_.yaw = yaw, user_.pitch = pitch;
  if (reset) reanchor(pos);
  if (!have_pose_) first_pose_t_ = s.t_ns;
  have_pose_ = true;
  last_pose_t_ = s.t_ns;
  log_.start(s.t_ns);
  if (tracking_ == 1) {
    map_.add_camera(pos);
    if (std::hypot(pos.x() - last_trav_.x(), pos.z() - last_trav_.z()) > 0.1) {
      nav_.mark_traversed(pos), last_trav_ = pos;
    }
    double h = pos.y() - map_.floor_y();
    if (h > 0.5 && h < 2.5) cam_height_ += 0.02 * (h - cam_height_);
  }
}

void GuideEngine::reanchor(const Vec3& pos) {
  // The reported pose may have jumped across the loss: speed / turn-rate estimates spanning it are meaningless (they
  // would trigger SLOW_DOWN), the traversal trail must not draw a line across the jump, and the plan was made from a
  // drifted pose. The engine's map, coverage and nav stay: ARCore re-anchors its world, it does not restart it.
  user_.speed = 0, user_.yaw_rate = 0;
  last_trav_ = {pos.x() + 1e9, 0, pos.z() + 1e9};  // the re-anchored spot is marked traversed now, with no trail to it
  last_plan_t_ = -1;  // replan on the next update()
}

void GuideEngine::on_gyro(const bundle::ImuSample& s) { rec_.on_gyro(s.t_ns, s.v[0], s.v[1], s.v[2]); }

void GuideEngine::refresh_levels() {
  if (map_.version() == map_version_) return;
  map_version_ = map_.version();
  nav_.set_floor_y(map_.floor_y());
  cov_.sync(map_);
}

void GuideEngine::assign_wall_ids() {
  for (auto& [id, pl] : map_.planes())
    if (pl.kind == "wall" && !wall_ids_.count(id)) wall_ids_[id] = next_wall_id_++;
}

void GuideEngine::on_plane(const bundle::PlaneRecord& r) {
  map_.add_plane(r);
  refresh_levels();
  assign_wall_ids();
}

void GuideEngine::on_detections(const bundle::DetectionsRecord& r) {
  if (p_.understanding == 0 || r.boxes.empty() || !have_K_ || track_.size() == 0) return;
  // A detection stamped just after the newest pose (the detector finishes before the next pose arrives) or just before
  // the oldest one takes the nearest pose within 50 ms instead of being dropped.
  std::optional<geometry::PoseWC> T = track_.interpolate_at(r.t_ns, 100'000'000);
  if (!T)
    if (auto i = track_.nearest(r.t_ns, 50'000'000)) T = track_.pose(*i);
  if (!T) return;
  LiftContext lc;
  lc.T_world_from_camera = *T, lc.K = K_, lc.has_floor = map_.has_floor(), lc.floor_y = map_.floor_y();
  for (auto& [id, pl] : map_.planes()) {
    if (!pl.surface()) continue;
    PlaneGeom g;
    g.c = pl.c, g.n = pl.n, g.area = pl.eu * pl.ev;
    lc.surfaces.push_back(g);
  }
  bool changed = false;
  for (auto& b : r.boxes) {
    if (b.cls <= 0 || b.cls >= int32_t(ObjectClass::Count_)) continue;
    if (!std::isfinite(b.score + b.x0 + b.y0 + b.x1 + b.y1)) continue;
    Detection d;
    d.t_ns = r.t_ns, d.frame_id = r.frame_id, d.cls = ObjectClass(b.cls), d.score = b.score;
    d.x0 = b.x0, d.y0 = b.y0, d.x1 = b.x1, d.y1 = b.y1;
    changed |= lifter_.on_detection(d, lc, pts_);
  }
  if (!changed) return;
  objects_ = lifter_.objects();
  nav_.clear_objects();  // re-stamp every object: a moved or resized one leaves no stale cells
  for (auto& o : objects_) nav_.add_object_box(o);
}

void GuideEngine::on_points(const bundle::PointCloudRecord& r) {
  points_seen_ = true;
  for (size_t i = 0; i + 2 < r.xyz.size(); i += 3) {
    Vec3 q(r.xyz[i], r.xyz[i + 1], r.xyz[i + 2]);
    nav_.add_point(q);
    if (have_pose_ && tracking_ == 1) nav_.carve(user_.pos, q);  // the sight ray to a seen point is free space
    pts_.add(q);  // layout evidence at every level (ADR 0026); objects / openings still need understanding
  }
  if (p_.understanding != 0 && (last_open_t_ < 0 || r.t_ns - last_open_t_ >= 1'000'000'000)) {
    last_open_t_ = r.t_ns;
    openings_ = opener_.detect(map_, pts_, [this](const std::string& id, const Vec3& p) { return cov_.views_at(id, p); });
  }
}

void GuideEngine::on_frame(const FrameObs& f) {
  blur_streak_ = (f.hard_reject && f.motion_blur) ? blur_streak_ + 1 : 0;
  if (f.underexposed) last_underexp_t_ = f.t_ns, underexp_seen_ = true;
}

void GuideEngine::on_keyframe(uint32_t, int64_t t, double sharp) {
  // While recovering the poses are not trusted: a view must not count as coverage (live, the session does not even
  // select keyframes then; in replay the quality sink still does, so the engine gates it the same way).
  if (track_.size() == 0 || rec_.active()) return;
  auto T = track_.interpolate_at(t, 100'000'000);
  if (!T) return;
  // Overlay v2: record the heading of this keyframe (needs only a pose, not intrinsics or planes). Looking nearly
  // straight up or down makes yaw meaningless, so those views do not count.
  if (std::fabs(pitch_of(*T)) < deg2rad(70)) {
    double deg = std::fmod(rad2deg(yaw_of(*T)), 360.0);
    if (deg < 0) deg += 360.0;
    int sector = std::min(int(deg / 30.0), OverlayModel::kYawSectors - 1);
    yaw_mask_ |= 1u << sector;
  }
  if (pitch_of(*T) >= deg2rad(p_.pitch_up_deg)) ceiling_look_ = true;  // overlay v3 ceiling_seen
  if (!have_K_) return;
  std::vector<Vec3> floor_seen;
  Obs o{*T, K_, t, sharp, true};
  cov_.observe(o, &nav_, &floor_seen);
  for (auto& p : floor_seen) nav_.mark_open(p);
  if (steer()) vcov_.observe(o, nullptr, nullptr);  // "did the user look there" for hypothesised surfaces
}

void GuideEngine::update(int64_t t, std::vector<Instruction>& out) {
  refresh_layout(t);
  if (!have_pose_ || !have_K_) return;
  const int64_t period = int64_t(1e9 / std::max(0.05, p_.replan_hz * rate_scale_));
  // No planning from untrusted poses while recovering: the plan is frozen and recomputed right after the reset.
  if (!rec_.active() && (last_plan_t_ < 0 || t - last_plan_t_ >= period)) {
    last_plan_t_ = t;
    int before = planner_.switches();
    PlanHints hints;
    hints.need_ceiling = !map_.has_ceiling() && map_.surface_count() >= 2;
    hints.need_floor = !map_.has_floor() && map_.surface_count() >= 2;
    if (steer()) {
      refresh_steer(t);
      for (auto& g : gaps_) {
        if (!gap_steers(g.type)) continue;
        GapTarget gt;
        gt.type = int(g.type), gt.x = 0.5 * (g.x0 + g.x1), gt.z = 0.5 * (g.z0 + g.z1);
        gt.y = g.type == GapType::Occluded ? 0.3 : 1.0;  // look at the floor behind an object / at eye-ish level on a wall
        gt.len = std::hypot(g.x1 - g.x0, g.z1 - g.z0), gt.pri = g.pri, gt.why = g.why;
        if (std::isfinite(gt.x + gt.z + gt.pri)) hints.gaps.push_back(gt);
      }
    }
    plan_ = planner_.replan(t, user_, cov_, nav_, K_, cam_height_, hints);
    if (planner_.switches() != before) log_.note_target_switch();
    log_.note_plan(plan_.target.valid);
    // Safety invariant: a WALK target is never an unknown or blocked cell. A discovery look (tilt where the
    // user already stands) asks for no movement, so it is not subject to it.
    if (plan_.target.valid && plan_.target_changed && !plan_.discover) {
      int ix, iz;
      nav_.to_cell({plan_.target.x, 0, plan_.target.z}, ix, iz);
      if (!nav_.passable(ix, iz)) log_.note_unsafe_target();
    }
    auto ev = cov_.eval_view(pose_from_yaw_pitch({user_.pos.x(), user_.pos.y(), user_.pos.z()}, user_.yaw, user_.pitch), K_, &nav_, false);
    too_close_ = ev.too_close;
  }
  if (last_sample_t_ < 0 || t - last_sample_t_ >= 1'000'000'000) {
    last_sample_t_ = t;
    auto st = cov_.stats();
    log_.sample(t, st.completeness(), st.seen_fraction());
  }
  Situation s;
  s.t_ns = t;
  s.user = user_;
  s.tracking_state = tracking_;
  s.tracking_reason = tracking_reason_;
  s.lost_since_ns = lost_since_;
  s.blur_streak = blur_streak_;
  s.underexposed = underexp_seen_ && t - last_underexp_t_ < 2'000'000'000;
  s.plan = plan_;
  s.too_close = too_close_;
  s.completeness = plan_.completeness;
  s.recovering = rec_.active();
  scratch_.clear();
  life_.clear();
  gen_.update(s, scratch_, life_);
  for (auto& e : life_) {
    log_.ended(e);
    // A movement/turn the user never completed means the target is unreachable for them: do not insist.
    if (e.state == LifeState::Expired && (e.ins.type == InstrType::MoveTo || e.ins.type == InstrType::RotateToward)) {
      planner_.penalize_current();
      plan_.target = planner_.current();
      last_plan_t_ = -1;  // re-plan immediately
    }
  }
  for (auto& i : scratch_) {
    log_.issued(i, s.completeness);
    out.push_back(i);
    ++prompts_;
  }
  if ((last_tl_t_ < 0 || t - last_tl_t_ >= 5'000'000'000) && timeline_.size() < kMaxTimeline) {
    last_tl_t_ = t;
    TimelineSample ts;
    ts.t_s = std::round(double(t - first_pose_t_) / 1e8) / 10.0 + 0.0;
    ts.completeness = std::round(cov_.stats().completeness() * 1e4) / 1e4;
    ts.walls = shell_.evidenced_walls();
    ts.prompts = prompts_;
    timeline_.push_back(ts);
  }
}

std::string GuideEngine::report_json() const {
  auto st = cov_.stats();
  json::Object o;
  o.set("params_hash", p_.hash()).set("tier", std::string(1, tier()));
  json::Object c;
  c.set("completeness", std::round(st.completeness() * 1e4) / 1e4).set("seen", std::round(st.seen_fraction() * 1e4) / 1e4)
      .set("valid_cells", uint64_t(st.valid)).set("planes", uint64_t(map_.surface_count()));
  o.raw("coverage", c.dump());
  json::Object n;
  n.set("obstacle_cells", uint64_t(nav_.obstacle_cells())).set("free_cells", uint64_t(nav_.free_cells())).set("passable_cells", uint64_t(nav_.passable_cells())).set("floor_y", std::round(map_.floor_y() * 1e3) / 1e3);
  o.raw("nav", n.dump());
  json::Object pl;
  pl.set("target_x", std::round(planner_.current().x * 100) / 100).set("target_z", std::round(planner_.current().z * 100) / 100).set("target_gain", std::round(planner_.current().gain * 10) / 10).set("switches", planner_.switches()).set("targets_reached", planner_.reached_count());
  o.raw("planner", pl.dump()).raw("log", log_.summary_json());
  // v3 (ADR 0026). The final report reflects the final map AND evidence: the throttled layout may predate the last
  // plane, point or nav change, so it is always recomputed, on a COPY (report_json stays const, engine state untouched).
  RoomShell fresh = shell_;
  {
    LayoutEvidence ev{&pts_, &nav_};
    fresh.update(map_, &wall_ids_, openings_, &ev);
  }
  const RoomShell* lay = &fresh;
  json::Object lj;
  lj.set("walls", int64_t(lay->evidenced_walls())).set("closed", lay->closed_with_evidence())
      .set("blocked_m", std::round(lay->blocked_m() * 100) / 100 + 0.0);
  o.raw("layout", lj.dump());
  std::vector<std::string> tl;
  for (auto& s : timeline_) {
    json::Object j;
    j.set("t_s", s.t_s).set("completeness", s.completeness).set("walls", int64_t(s.walls)).set("prompts", s.prompts);
    tl.push_back(j.dump());
  }
  o.raw("timeline", json::array(tl.begin(), tl.end()));
  if (p_.understanding != 0 && !objects_.empty()) {  // absent otherwise: flag-off and no-detection reports stay as they were
    auto r2 = [](double v) { return std::round(v * 100.0) / 100.0 + 0.0; };
    std::vector<std::string> arr;
    for (auto& b : objects_) {
      json::Object j;
      j.set("id", int64_t(b.id)).set("cls", std::string(object_class_name(b.cls))).set("x", r2(b.centre.x())).set("z", r2(b.centre.z()))
          .set("w", r2(b.size.x())).set("d", r2(b.size.z())).set("h", r2(b.size.y())).set("yaw_deg", std::round(rad2deg(b.yaw)) + 0.0)
          .set("conf", r2(b.conf)).set("n_obs", int64_t(b.n_obs));
      arr.push_back(j.dump());
    }
    o.raw("objects", json::array(arr.begin(), arr.end()));
  }
  if (rec_.losses() > 0) {  // absent when tracking never stayed degraded for enter_s: existing reports are unchanged
    json::Object rj;
    rj.set("losses", int64_t(rec_.losses())).set("resets", int64_t(rec_.resets()))
        .set("max_drift_m", std::round(rec_.max_drift_m() * 1000) / 1000 + 0.0).set("active", rec_.active());
    o.raw("recovery", rj.dump());
  }
  if (steer()) {  // absent at 0 and 1: their reports (and the replay baselines) are unchanged
    json::Object sj;
    sj.set("gaps", uint64_t(gaps_.size())).set("gap_targets", int64_t(planner_.gap_picks())).set("shell_walls", uint64_t(shell_.shell().walls.size()))
        .set("shell_closed", shell_.shell().closed);
    o.raw("steer", sj.dump());
  }
  return o.dump();
}

}  // namespace capturekit::guide

namespace capturekit::guide {

OverlayModel GuideEngine::overlay(int64_t t) const {
  OverlayModel m;
  m.t_ns = t;
  auto st = cov_.stats();
  m.completeness = st.completeness();
  m.seen = st.seen_fraction();
  m.tracking = tracking_;
  m.mode = plan_.mode == Mode::Finish ? "finish" : plan_.mode == Mode::Target ? "target" : "sweep";
  m.floor_found = map_.has_floor();
  m.ceiling_found = map_.has_ceiling();
  m.user_x = user_.pos.x(), m.user_z = user_.pos.z(), m.user_yaw_deg = rad2deg(user_.yaw);
  m.has_target = plan_.mode == Mode::Target && plan_.target.valid;
  if (m.has_target) m.target_x = plan_.target.x, m.target_z = plan_.target.z;
  // Overlay v3 (ADR 0026): walls are LAYOUT walls (lines), not planes.
  for (auto& [id, pl] : map_.planes()) m.wall_found = m.wall_found || pl.kind == "wall";
  m.walls = shell_.evidenced_walls();
  m.ceiling_seen = ceiling_seen();
  const ShellModel& lay = shell_.shell();
  std::vector<char> wall_ev;  // parallel to m.wall_list: the wall has evidence (planes or points)
  for (size_t i = 0; i < lay.walls.size() && m.wall_list.size() < OverlayModel::kMaxWalls; ++i) {
    const ShellWall& sw = lay.walls[i];
    const LayoutWall& lw = shell_.layout_walls()[i];
    OverlayWall w;
    w.x0 = sw.x0, w.z0 = sw.z0, w.x1 = sw.x1, w.z1 = sw.z1;
    if (!std::isfinite(w.x0 + w.z0 + w.x1 + w.z1)) continue;  // never emit NaN/inf coordinates
    size_t valid = 0, seen = 0;
    for (auto& pid : lw.planes) {
      auto cs = cov_.stats(pid);
      valid += cs.valid, seen += cs.seen;
    }
    if (lw.planes.empty() && steer()) {
      auto cs = vcov_.stats("v:" + sw.id);
      valid += cs.valid, seen += cs.seen;
    }
    const double sf = valid ? double(seen) / double(valid) : 0.0;
    w.seen_pct = int(std::lround(std::clamp(100.0 * sf, 0.0, 100.0)));
    w.id = lw.num;
    m.wall_list.push_back(w);
    wall_ev.push_back(lw.evidenced ? 1 : 0);
  }
  for (auto& sp : limit_spans(shell_.spans(), OverlayModel::kMaxSpans))  // a fair share per wall: later walls keep theirs
    m.span_list.push_back({layout_wall_num(sp.wall), sp.x0, sp.z0, sp.x1, sp.z1, sp.state});
  for (auto& c : lay.corners)
    if (m.shell_corners.size() < OverlayModel::kMaxCorners) m.shell_corners.push_back(c);
  m.shell_closed = shell_.closed_with_evidence() && !m.shell_corners.empty();
  for (auto& b : objects_) {  // empty unless understanding is on and detections were fed
    if (m.object_list.size() >= OverlayModel::kMaxObjects) break;
    OverlayObject ob;
    ob.id = int(b.id), ob.cls = object_class_name(b.cls);
    ob.x = b.centre.x(), ob.z = b.centre.z(), ob.w = b.size.x(), ob.d = b.size.z(), ob.h = b.size.y();
    double deg = std::fmod(rad2deg(b.yaw), 90.0);
    ob.yaw_deg = int(std::lround(deg < 0 ? deg + 90.0 : deg)) % 90;
    m.object_list.push_back(ob);
  }
  if (steer()) {  // Phase 9 step 8: only what exists, capped; empty lists emit no JSON key
    for (auto& op : shell_.openings()) {
      if (m.opening_list.size() >= OverlayModel::kMaxOpenings) break;
      m.opening_list.push_back({op.kind == OpeningKind::Door ? "door" : "window", op.wall, op.x0, op.z0, op.x1, op.z1, op.y0, op.y1});
    }
    for (auto& g : gaps_) {
      if (m.gap_list.size() >= OverlayModel::kMaxGaps) break;
      m.gap_list.push_back({gap_type_name(g.type), 0.5 * (g.x0 + g.x1), 0.5 * (g.z0 + g.z1), g.pri});
    }
    if (!gaps_.empty()) m.gap_why_id = std::string("gap.") + gap_type_name(gaps_.front().type), m.gap_why_text = gaps_.front().why;
  }
  m.yaw_mask = yaw_mask_;

  // The one most useful reason right now, chosen in priority order.
  const char* why = "why.keep";
  const bool dark = underexp_seen_ && t - last_underexp_t_ < 2'000'000'000;
  m.recalibrating = rec_.active();
  if (m.recalibrating) {
    why = "why.recalibrating";
  } else if (tracking_ == 3) {
    why = "why.tracking_lost";
  } else if (tracking_ == 2) {
    why = "why.limited";
  } else if (blur_streak_ >= int(p_.blur_frames)) {
    why = "why.too_fast";
  } else if (dark) {
    why = "why.too_dark";
  } else if (plan_.mode == Mode::Finish) {
    why = "why.finish";
  } else if (plan_.mode == Mode::Sweep) {
    why = "why.sweep";
  } else if (plan_.discover && plan_.target.valid && plan_.target.pitch > 0 && !map_.has_ceiling()) {
    why = "why.ceiling";
  } else if (plan_.discover && plan_.target.valid && plan_.target.pitch <= 0 && !map_.has_floor()) {
    why = "why.floor";
  } else {
    const OverlayWall* worst = nullptr;
    for (size_t i = 0; i < m.wall_list.size(); ++i) {  // only evidenced walls: a hypothesised side is not "the wall ahead"
      const OverlayWall& w = m.wall_list[i];
      if (wall_ev[i] && w.seen_pct < 80 && (!worst || w.seen_pct < worst->seen_pct)) worst = &w;
    }
    if (worst) {
      double rel = wrap_pi(std::atan2(0.5 * (worst->x0 + worst->x1) - user_.pos.x(), 0.5 * (worst->z0 + worst->z1) - user_.pos.z()) - user_.yaw);  // + = left
      double a = std::fabs(rel);
      why = a < deg2rad(45) ? "why.wall_ahead" : a > deg2rad(135) ? "why.wall_behind" : rel > 0 ? "why.wall_left" : "why.wall_right";
    } else if (!map_.has_ceiling() && map_.surface_count() >= 2) {
      why = "why.ceiling";
    } else if (!map_.has_floor() && map_.surface_count() >= 2) {
      why = "why.floor";
    }
  }
  m.why_id = why;
  m.why_text = why_line(why);
  return m;
}

}  // namespace capturekit::guide
