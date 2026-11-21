#include "capturekit/guide/planner.hpp"

#include <algorithm>
#include <cmath>

namespace capturekit::guide {

double Planner::revisit_factor(double x, double z, double yaw) const {
  double f = 1;
  for (auto& v : visited_)
    if (std::hypot(v.x - x, v.z - z) < 0.6 && std::fabs(wrap_pi(v.yaw - yaw)) < deg2rad(35)) f *= std::pow(p_.revisit_decay, v.n);
  return f;
}

int Planner::gap_visit_count(int type, double x, double z) const {
  for (auto& v : gap_visits_)
    if (v.type == type && std::hypot(v.x - x, v.z - z) < sp_.gap_match_m) return v.n;
  return 0;
}

void Planner::gap_visited(int type, double x, double z) {
  for (auto& v : gap_visits_)
    if (v.type == type && std::hypot(v.x - x, v.z - z) < sp_.gap_match_m) return void(++v.n);
  gap_visits_.push_back({type, x, z, 1});
}

const GapTarget* Planner::find_gap(const std::vector<GapTarget>& gaps, int type, double x, double z) const {
  const GapTarget* best = nullptr;
  double bd = sp_.gap_match_m;
  for (auto& g : gaps) {
    double d = std::hypot(g.x - x, g.z - z);
    if (g.type == type && d < bd) bd = d, best = &g;
  }
  return best;
}

double Planner::gap_score(const UserState& u, double x, double z, const GapTarget& g, const NavGrid& nav,
                          const std::vector<float>& dist, double cam_h, double* path_out) const {
  int ix = int(std::floor(x / nav.cell())), iz = int(std::floor(z / nav.cell()));
  if (!nav.inside(ix, iz) || dist.empty()) return -1;
  const float d = dist[nav.index(ix, iz)];
  const double clr = nav.clearance(ix, iz);
  const double dg = std::hypot(g.x - x, g.z - z);
  if (d < 0 || clr < sp_.unseen_clear_m || dg < sp_.gap_dist_min || dg > sp_.gap_dist_max) return -1;
  if (!nav.clear(Vec3(x, nav.floor_y() + cam_h, z), Vec3(g.x, nav.floor_y() + g.y, g.z))) return -1;
  const double yaw = std::atan2(g.x - x, g.z - z);
  const double turn = std::fabs(wrap_pi(yaw - u.yaw)) * p_.turn_cost_m_per_rad;
  if (path_out) *path_out = d;
  return sp_.gap_gain * g.pri / (double(d) + turn + p_.instr_overhead_m) * revisit_factor(x, z, yaw);
}

double Planner::score_view(const UserState& u, double x, double z, double yaw, double pitch, const Coverage& cov,
                           const NavGrid& nav, const std::vector<float>& dist, const geometry::Intrinsics& K,
                           double cam_h, double* gain_out, double* path_out) const {
  int ix = int(std::floor(x / nav.cell())), iz = int(std::floor(z / nav.cell()));
  if (!nav.inside(ix, iz)) return -1;
  float d = dist.empty() ? -1.f : dist[nav.index(ix, iz)];
  if (d < 0) return -1;
  Vec3 pos(x, nav.floor_y() + cam_h, z);
  ViewEval e = cov.eval_view(pose_from_yaw_pitch(pos, yaw, pitch), K, &nav, false);
  double turn = std::fabs(wrap_pi(yaw - u.yaw)) * p_.turn_cost_m_per_rad;
  double clr = nav.clearance(ix, iz);
  double safety = clr < p_.safety_margin_m ? p_.safety_penalty_m * (1 - clr / p_.safety_margin_m) : 0;
  if (gain_out) *gain_out = e.gain;
  if (path_out) *path_out = d;
  // instr_overhead_m: every instruction costs a person a reaction delay, so tiny moves are not free
  return e.gain / (double(d) + turn + safety + p_.instr_overhead_m) * revisit_factor(x, z, yaw);
}

void Planner::penalize_current() {
  if (!cur_.valid) return;
  bool found = false;
  for (auto& v : visited_)
    if (std::hypot(v.x - cur_.x, v.z - cur_.z) < 0.6 && std::fabs(wrap_pi(v.yaw - cur_.yaw)) < deg2rad(35)) v.n += 3, found = true;
  if (!found) visited_.push_back({cur_.x, cur_.z, cur_.yaw, 3});
  cur_ = {};
}

Plan Planner::replan(int64_t t, const UserState& u, const Coverage& cov, NavGrid& nav, const geometry::Intrinsics& K,
                     double cam_h, const PlanHints& hints) {
  Plan plan;
  auto st = cov.stats();
  plan.completeness = st.completeness();
  plan.target = cur_;
  nav.rebuild();

  if (st.valid == 0) {
    cur_ = {};
    plan.target = cur_;
    plan.mode = Mode::Sweep;  // no map yet (tier C): ask for a slow sweep
    return plan;
  }
  const bool discover_possible = discover_tries_ < int(p_.discover_tries) && (hints.need_ceiling || hints.need_floor);
  // Steering: gaps still open (not stood at max_gap_visits times) defer Finish; only gap views are then considered.
  std::vector<const GapTarget*> open;
  if (steer_on_)
    for (auto& g : hints.gaps)
      if (gap_visit_count(g.type, g.x, g.z) < sp_.max_gap_visits) open.push_back(&g);
  const bool gap_only = steer_on_ && !open.empty() && plan.completeness >= p_.complete_threshold && !discover_possible;
  if (plan.completeness >= p_.complete_threshold && !discover_possible && !gap_only) {
    cur_ = {};
    plan.target = cur_;
    plan.mode = Mode::Finish;
    return plan;
  }

  int sx, sz;
  nav.to_cell(u.pos, sx, sz);
  if (!nav.passable(sx, sz) && !nav.nearest_passable(sx, sz, 1.0, sx, sz)) {
    cur_ = {};
    plan.target = cur_;
    plan.mode = Mode::Sweep;  // no known free space around the user yet
    return plan;
  }
  std::vector<float> dist;
  nav.distances(sx, sz, dist);

  // Current target: reached, obsolete, or rescored for hysteresis.
  double cur_score = 0;
  if (gap_only && cur_.valid && !cur_.gap) cur_ = {};  // coverage is done: only gap views remain worth a walk
  if (cur_.valid && cur_.gap) {
    const GapTarget* gt = find_gap(hints.gaps, cur_.gap_type, cur_.gap_x, cur_.gap_z);
    const bool is_open = gt && gap_visit_count(gt->type, gt->x, gt->z) < sp_.max_gap_visits;
    const double dh = std::hypot(cur_.x - u.pos.x(), cur_.z - u.pos.z());
    const bool at_pose = dh <= p_.reach_m && std::fabs(wrap_pi(cur_.yaw - u.yaw)) <= deg2rad(p_.reach_yaw_deg) &&
                         std::fabs(cur_.pitch - u.pitch) <= deg2rad(20);
    if (at_pose && cur_.at_since < 0) cur_.at_since = t;
    if (!at_pose) cur_.at_since = -1;
    const bool dwelled = cur_.at_since >= 0 && double(t - cur_.at_since) / 1e9 >= p_.dwell_s;
    if (!is_open || (at_pose && dwelled)) {
      // The gap closed (it was looked at) or the user stood and looked here long enough: this visit is over.
      if (at_pose) {
        if (is_open) gap_visited(gt->type, gt->x, gt->z);
        bool found = false;
        for (auto& v : visited_)
          if (std::hypot(v.x - cur_.x, v.z - cur_.z) < 0.6 && std::fabs(wrap_pi(v.yaw - cur_.yaw)) < deg2rad(35)) ++v.n, found = true;
        if (!found) visited_.push_back({cur_.x, cur_.z, cur_.yaw, 1});
        ++reached_total_;
        plan.reached = true;
      }
      cur_ = {};
    } else {
      double path = 0;
      cur_score = gap_score(u, cur_.x, cur_.z, *gt, nav, dist, cam_h, &path);
      if (cur_score < 0) cur_ = {}, cur_score = 0;  // no longer in line of sight / reachable
      else cur_.score = cur_score;
    }
  } else if (cur_.valid) {
    double dh = std::hypot(cur_.x - u.pos.x(), cur_.z - u.pos.z());
    const bool at_pose = dh <= p_.reach_m && std::fabs(wrap_pi(cur_.yaw - u.yaw)) <= deg2rad(p_.reach_yaw_deg) &&
                         std::fabs(cur_.pitch - u.pitch) <= deg2rad(20);
    double g = 0, path = 0;
    cur_score = score_view(u, cur_.x, cur_.z, cur_.yaw, cur_.pitch, cov, nav, dist, K, cam_h, &g, &path);
    if (at_pose && cur_.at_since < 0) cur_.at_since = t;
    if (!at_pose) cur_.at_since = -1;
    // "Reached" means the view was realised (its gain mostly collected) or the user has dwelled there long
    // enough: standing at the pose before any keyframe was taken is not yet a visit.
    const bool realized = cur_.gain0 > 0 && g < p_.realized_gain * cur_.gain0;
    const bool dwelled = cur_.at_since >= 0 && double(t - cur_.at_since) / 1e9 >= p_.dwell_s;
    if (at_pose && (realized || dwelled || cur_.gain0 <= 0)) {
      bool found = false;
      for (auto& v : visited_)
        if (std::hypot(v.x - cur_.x, v.z - cur_.z) < 0.6 && std::fabs(wrap_pi(v.yaw - cur_.yaw)) < deg2rad(35)) ++v.n, found = true;
      if (!found) visited_.push_back({cur_.x, cur_.z, cur_.yaw, 1});
      ++reached_total_;
      plan.reached = true;
      if (cur_.fallback && cur_.gain < 0) ++discover_tries_;  // a discovery look was carried out
      cur_ = {};
      cur_score = 0;
    } else if (cur_score < 0 || g < p_.min_gain) {
      cur_ = {};  // unreachable now, or nothing left to gain
      cur_score = 0;
    } else {
      cur_.gain = g;
    }
  }

  if (steer_on_)  // a visit just ended may have used up a gap
    open.erase(std::remove_if(open.begin(), open.end(), [&](const GapTarget* g) { return gap_visit_count(g->type, g->x, g->z) >= sp_.max_gap_visits; }),
               open.end());

  // Candidate search.
  const int L = std::max(1, int(std::lround(p_.lattice_m / nav.cell())));
  const int nh = std::max(1, int(p_.headings));
  struct Cand {
    double x, z, yaw, pitch, score, gain, path;
    int gi = -1;  // >= 0: a gap view, index into `open`
  };
  std::vector<Cand> top, topf;  // topf: steering only, the candidates that pass the hop / clearance filter
  auto keep = [](std::vector<Cand>& v, const Cand& c) {
    v.push_back(c);
    std::sort(v.begin(), v.end(), [](const Cand& a, const Cand& b) {
      if (a.score != b.score) return a.score > b.score;
      if (a.x != b.x) return a.x < b.x;
      if (a.z != b.z) return a.z < b.z;
      if (a.yaw != b.yaw) return a.yaw < b.yaw;
      return a.pitch < b.pitch;
    });
    if (v.size() > 8) v.pop_back();
  };
  auto consider = [&](const Cand& c) {
    keep(top, c);
    if (steer_on_ && (c.path <= p_.reach_m || c.path >= sp_.min_hop_m) &&
        nav.clearance(int(std::floor(c.x / nav.cell())), int(std::floor(c.z / nav.cell()))) >= sp_.unseen_clear_m)
      keep(topf, c);
  };
  for (int iz = nav.z0(); !gap_only && iz < nav.z0() + nav.nz(); ++iz) {
    if (((iz % L) + L) % L) continue;
    for (int ix = nav.x0(); ix < nav.x0() + nav.nx(); ++ix) {
      if (((ix % L) + L) % L) continue;
      if (dist[nav.index(ix, iz)] < 0) continue;
      Vec3 w = nav.to_world(ix, iz);
      std::vector<std::pair<double, double>> hs;  // (score, yaw) for pitch 0
      for (int k = 0; k < nh; ++k) {
        double yaw = wrap_pi(2 * kPi * k / nh), g = 0, pa = 0;
        double s = score_view(u, w.x(), w.z(), yaw, 0, cov, nav, dist, K, cam_h, &g, &pa);
        hs.push_back({s, yaw});
        if (s > 0) consider({w.x(), w.z(), yaw, 0, s, g, pa});
      }
      std::sort(hs.begin(), hs.end(), [](auto& a, auto& b) { return a.first > b.first; });
      for (int h = 0; h < 2 && h < int(hs.size()); ++h) {  // pitch variants only for the 2 best headings
        if (hs[size_t(h)].first <= 0) break;
        for (double pitch : {deg2rad(p_.pitch_up_deg), -deg2rad(p_.pitch_up_deg), -deg2rad(1.8 * p_.pitch_up_deg)}) {
          double g = 0, pa = 0;
          double s = score_view(u, w.x(), w.z(), hs[size_t(h)].second, pitch, cov, nav, dist, K, cam_h, &g, &pa);
          if (s > 0) consider({w.x(), w.z(), hs[size_t(h)].second, pitch, s, g, pa});
        }
      }
    }
  }

  // Gap views (steering): for each open gap the best lattice cell that sees it from the viewing band.
  const bool gaps_now = gap_only || plan.completeness >= sp_.gap_after || top.empty();
  for (size_t gi = 0; gaps_now && gi < open.size(); ++gi) {
    const GapTarget& g = *open[gi];
    Cand best{};
    bool have = false;
    for (int iz = nav.z0(); iz < nav.z0() + nav.nz(); ++iz) {
      if (((iz % L) + L) % L) continue;
      for (int ix = nav.x0(); ix < nav.x0() + nav.nx(); ++ix) {
        if (((ix % L) + L) % L || dist[nav.index(ix, iz)] < 0) continue;
        Vec3 w = nav.to_world(ix, iz);
        double pa = 0, s = gap_score(u, w.x(), w.z(), g, nav, dist, cam_h, &pa);
        if (s <= 0 || !(pa <= p_.reach_m || pa >= sp_.min_hop_m)) continue;
        if (!have || s > best.score) best = {w.x(), w.z(), std::atan2(g.x - w.x(), g.z - w.z()), 0, s, sp_.gap_gain * g.pri, pa, int(gi)}, have = true;
      }
    }
    if (have) keep(top, best), keep(topf, best);  // already filtered
  }

  // Overlap constraint on the few best candidates (tracking safety), then pick. Steering prefers the filtered list.
  auto choose = [&](const std::vector<Cand>& list, std::vector<Cand>& ok) -> const Cand* {
    for (auto& c : list) {
      if (c.gain < p_.min_gain) continue;
      if (c.gi >= 0) {  // a gap view looks at what the map lacks: no overlap test
        ok.push_back(c);
        continue;
      }
      ViewEval e = cov.eval_view(pose_from_yaw_pitch({c.x, nav.floor_y() + cam_h, c.z}, c.yaw, c.pitch), K, &nav, true);
      if (e.overlap >= p_.overlap_min) ok.push_back(c);
    }
    if (!ok.empty()) return &ok.front();
    if (!list.empty() && list.front().gain >= p_.min_gain) return &list.front();
    return nullptr;
  };
  std::vector<Cand> ok, okf;
  const Cand* pick = nullptr;
  if (steer_on_) pick = choose(topf, okf);
  if (!pick) pick = choose(top, ok);

  if (gap_only && !(pick && pick->gi >= 0) && !(cur_.valid && cur_.gap)) {
    cur_ = {};  // coverage is complete and no gap can be viewed: finished
    plan.target = cur_;
    plan.mode = Mode::Finish;
    return plan;
  }

  // Discovery: once the known surfaces are mostly done (or nothing else is worth doing), ask the user to
  // look at the missing ceiling / floor so those planes get detected (they then join normal coverage).
  if (!cur_.valid && discover_possible && (!pick || plan.completeness >= p_.discover_after)) {
    bool ceiling = hints.need_ceiling;
    double pitch = deg2rad(ceiling ? p_.discover_pitch_deg : -1.4 * p_.discover_pitch_deg);
    cur_ = {true, true, u.pos.x(), u.pos.z(), u.yaw, pitch, -1, 0, 0, t, -1, next_id_++};  // gain -1 marks 'discovery'
    plan.target_changed = true;
    plan.discover = true;
    pick = nullptr;
  }
  if (pick && (!cur_.valid || pick->score > p_.hysteresis * cur_score)) {
    bool changed = cur_.valid;  // replacing a live target counts as a switch
    cur_ = {true, false, pick->x, pick->z, pick->yaw, pick->pitch, pick->gain, pick->score, pick->gain, t, -1, next_id_++};
    if (pick->gi >= 0) {
      const GapTarget& g = *open[size_t(pick->gi)];
      cur_.gap = true, cur_.gap_type = g.type, cur_.gap_x = g.x, cur_.gap_z = g.z, cur_.why = g.why;
      ++gap_picks_;
    }
    if (changed) ++switches_;
    plan.target_changed = true;
  } else if (!cur_.valid) {
    // Fallback: walk toward the unseen area (nearest passable cell to the deficient centroid).
    const auto& def = cov.deficient();
    if (!def.empty()) {
      Vec3 cen = Vec3::Zero();
      for (auto& s : def) cen += s.p;
      cen /= double(def.size());
      double bd = 1e18;
      int bx = sx, bz = sz;
      for (int iz = nav.z0(); iz < nav.z0() + nav.nz(); ++iz)
        for (int ix = nav.x0(); ix < nav.x0() + nav.nx(); ++ix) {
          if (dist[nav.index(ix, iz)] < 0) continue;
          Vec3 w = nav.to_world(ix, iz);
          double d = std::hypot(w.x() - cen.x(), w.z() - cen.z());
          if (d < bd) bd = d, bx = ix, bz = iz;
        }
      Vec3 w = nav.to_world(bx, bz);
      double yaw = std::atan2(cen.x() - w.x(), cen.z() - w.z());
      if (std::hypot(w.x() - u.pos.x(), w.z() - u.pos.z()) > p_.reach_m) {
        cur_ = {true, true, w.x(), w.z(), yaw, 0, 0, 0, 0, t, -1, next_id_++};
        plan.target_changed = true;
      }
    }
  }
  plan.target = cur_;
  if (cur_.valid) {
    int tx, tz;
    nav.to_cell({cur_.x, 0, cur_.z}, tx, tz);
    plan.path_m = nav.inside(tx, tz) ? std::max(0.f, dist[nav.index(tx, tz)]) : 0;
    plan.mode = Mode::Target;
  } else {
    plan.mode = Mode::Sweep;  // nothing reachable to gain: keep the user turning to reveal more
  }
  return plan;
}

}  // namespace capturekit::guide
