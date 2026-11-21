#include "capturekit/guide/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "capturekit/util/json.hpp"

namespace capturekit::guide {

namespace {
// Message catalog (locale "en") for the overlay "why" line; ids are stable translation keys.
const std::map<std::string, const char*>& catalog() {
  static const std::map<std::string, const char*> c = {
      {"why.ceiling", "Ceiling not seen yet"},
      {"why.finish", "Scan complete"},
      {"why.floor", "Floor not seen yet"},
      {"why.keep", "Keep scanning slowly"},
      {"why.limited", "Tracking weak: move slowly"},
      {"why.recalibrating", "Recalibrating: turn the phone slowly"},
      {"why.sweep", "Looking for walls: sweep slowly"},
      {"why.too_dark", "Too dark here"},
      {"why.too_fast", "Moving too fast"},
      {"why.tracking_lost", "Tracking lost"},
      {"why.wall_ahead", "Wall ahead not scanned yet"},
      {"why.wall_behind", "Wall behind you not scanned yet"},
      {"why.wall_left", "Left wall not scanned yet"},
      {"why.wall_right", "Right wall not scanned yet"},
  };
  return c;
}
double r2(double v) { return std::round(v * 100.0) / 100.0 + 0.0; }  // + 0.0 turns -0 into +0
double r4(double v) { return std::round(v * 1e4) / 1e4 + 0.0; }
}  // namespace

const char* why_line(const std::string& id) {
  auto it = catalog().find(id);
  return it == catalog().end() ? "" : it->second;
}

std::vector<std::string> why_ids() {
  std::vector<std::string> v;
  for (auto& [k, _] : catalog()) v.push_back(k);
  return v;
}

std::string OverlayModel::to_json() const {
  std::string ws;
  for (auto& w : wall_list) {
    if (!std::isfinite(w.x0 + w.z0 + w.x1 + w.z1)) continue;  // defence in depth: JSON has no NaN/inf
    char b[112];
    std::snprintf(b, sizeof b, "%.2f,%.2f,%.2f,%.2f,%d,%d", r2(w.x0), r2(w.z0), r2(w.x1), r2(w.z1),
                  std::clamp(w.seen_pct, 0, 100), std::clamp(w.id, -kMaxId, kMaxId));  // v3: -k = inferred wall I<k>
    if (!ws.empty()) ws += ";";
    ws += b;
  }
  std::string os;
  for (auto& b : object_list) {
    if (!std::isfinite(b.x) || !std::isfinite(b.z) || !std::isfinite(b.w) || !std::isfinite(b.d) || !std::isfinite(b.h)) continue;
    if (b.cls.size() > 24 || b.cls.find_first_of(",;\"\\") != std::string::npos) continue;
    auto lim = [](double v) { return std::clamp(v, -1e4, 1e4); };  // bounded width: an entry never truncates in `t`
    char t[160];
    std::snprintf(t, sizeof t, "%d,%s,%.2f,%.2f,%.2f,%.2f,%d,%.2f", std::max(b.id, 0), b.cls.c_str(), r2(lim(b.x)),
                  r2(lim(b.z)), r2(lim(b.w)), r2(lim(b.d)), std::clamp(b.yaw_deg, 0, 359), r2(lim(b.h)));
    if (!os.empty()) os += ";";
    os += t;
  }
  auto lim = [](double v) { return std::clamp(v, -1e4, 1e4); };
  auto clean = [](const std::string& s, size_t cap) { return s.size() <= cap && s.find_first_of(",;\"\\") == std::string::npos; };
  std::string ops;
  for (auto& p : opening_list) {
    if (!std::isfinite(p.x0 + p.z0 + p.x1 + p.z1 + p.y0 + p.y1) || !clean(p.kind, 8) || !clean(p.wall, 8)) continue;
    char t[160];
    std::snprintf(t, sizeof t, "%s,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", p.kind.c_str(), p.wall.c_str(), r2(lim(p.x0)), r2(lim(p.z0)),
                  r2(lim(p.x1)), r2(lim(p.z1)), r2(lim(p.y0)), r2(lim(p.y1)));
    if (!ops.empty()) ops += ";";
    ops += t;
  }
  std::string sh;
  for (auto& c : shell_corners) {
    if (!std::isfinite(c.first + c.second)) continue;
    char t[48];
    std::snprintf(t, sizeof t, "%.2f,%.2f", r2(lim(c.first)), r2(lim(c.second)));
    if (!sh.empty()) sh += ";";
    sh += t;
  }
  std::string sp;
  for (auto& s : span_list) {
    if (!std::isfinite(s.x0 + s.z0 + s.x1 + s.z1) || (s.state != 'o' && s.state != 'b' && s.state != 'm')) continue;
    char t[112];
    std::snprintf(t, sizeof t, "%d,%.2f,%.2f,%.2f,%.2f,%c", std::clamp(s.wall, -kMaxId, kMaxId), r2(lim(s.x0)), r2(lim(s.z0)),
                  r2(lim(s.x1)), r2(lim(s.z1)), s.state);
    if (!sp.empty()) sp += ";";
    sp += t;
  }
  std::string gs;
  for (auto& g : gap_list) {
    if (!std::isfinite(g.x + g.z + g.pri) || !clean(g.type, 24)) continue;
    char t[96];
    std::snprintf(t, sizeof t, "%s,%.2f,%.2f,%.2f", g.type.c_str(), r2(lim(g.x)), r2(lim(g.z)), r2(std::clamp(g.pri, 0.0, 1.0)));
    if (!gs.empty()) gs += ";";
    gs += t;
  }
  json::Object o;
  o.set("type", "overlay").set("t_ns", t_ns).set("completeness", r4(completeness)).set("seen", r4(seen))
      .set("tracking", tracking).set("mode", mode).set("wall_found", wall_found).set("floor_found", floor_found)
      .set("ceiling_found", ceiling_found).set("walls", walls).set("user_x", r2(user_x)).set("user_z", r2(user_z))
      .set("user_yaw_deg", std::round(user_yaw_deg) + 0.0).set("has_target", has_target)
      .set("target_x", r2(target_x)).set("target_z", r2(target_z)).set("why_id", why_id).set("why_text", why_text)
      .set("surfaces", ws).set("yaw_mask", int64_t(yaw_mask & ((1u << kYawSectors) - 1)))
      .set("spans", sp).set("shell", sh).set("shell_closed", shell_closed && !sh.empty()).set("ceiling_seen", ceiling_seen);
  if (!os.empty()) o.set("objects", os);
  if (!ops.empty()) o.set("openings", ops);
  if (recalibrating) o.set("recalibrating", true);  // absent otherwise: existing overlays (and goldens) are unchanged
  if (!gs.empty()) o.set("gaps", gs).set("gap_why_id", gap_why_id).set("gap_why_text", gap_why_text);
  return o.dump();
}

}  // namespace capturekit::guide
