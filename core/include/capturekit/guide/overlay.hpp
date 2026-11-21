#pragma once
// OverlayModel (Phase 11 / android brief B1): a small, flat snapshot of what the guidance core currently
// understands about the room, for glanceable UI. Built by GuideEngine::overlay(t_ns) from event time only
// (no clock), serialised as the non-critical session event {"type":"overlay",...} (see docs/guide.md).
// All user-facing "why" text comes from the core catalog here; shells render it verbatim.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace capturekit::guide {

/// A layout wall (overlay v3, ADR 0026): one wall LINE of the room layout (RoomShell), not one AR plane.
struct OverlayWall {
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;  // horizontal footprint endpoints (outline corners when closed), world metres
  int seen_pct = 0;                       // 0-100, seen fraction of the coverage cells of the wall's member planes
  int id = 0;                             // n of a "W<n>" wall (stable: smallest first-seen plane number wins, never
                                          // reused; UI says "Wall N"); -k for a plane-less inferred wall "I<k>"
};

/// A stretch of a layout wall (overlay v3): observed (a wall plane or wall points there), blocked (obstacles /
/// furniture in front of it: never demanded) or missing (nothing seen there yet).
struct OverlaySpan {
  int wall = 0;  // OverlayWall::id of its wall
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  char state = 'm';  // 'o' observed | 'b' blocked | 'm' missing
};

/// A lifted object (Phase 9, understanding on only): box centre on the floor plan, footprint w x d, height h.
struct OverlayObject {
  int id = 0;
  std::string cls;                           // object_class_name(), e.g. "chair"
  double x = 0, z = 0, w = 0, d = 0, h = 0;  // metres
  int yaw_deg = 0;                           // 0..89 (a box is symmetric modulo 90 degrees)
};

/// A door / window on a shell wall (Phase 9, understanding >= 2 only).
struct OverlayOpening {
  std::string kind, wall;  // "door" | "window"; wall id such as "W1" ("" when none)
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0, y0 = 0, y1 = 0;
};

/// An expected-but-unobserved area (Phase 9, understanding >= 2 only), highest priority first.
struct OverlayGap {
  std::string type;  // gap_type_name(), e.g. "missing_wall"
  double x = 0, z = 0, pri = 0;
};

struct OverlayModel {
  int64_t t_ns = 0;
  double completeness = 0, seen = 0;     // 0..1
  int tracking = 1;                      // 1 normal, 2 limited, 3 not available
  std::string mode = "sweep";            // sweep | target | finish
  bool wall_found = false, floor_found = false, ceiling_found = false;  // map flags (any wall / floor / ceiling plane)
  bool ceiling_seen = false;             // v3: a ceiling plane was found or a keyframe looked up >= pitch_up_deg
  int walls = 0;                         // v3: layout walls with evidence (observed or point-inferred), NOT planes
  double user_x = 0, user_z = 0, user_yaw_deg = 0;  // positive yaw = left
  bool has_target = false;
  double target_x = 0, target_z = 0;
  std::string why_id, why_text;
  std::vector<OverlayWall> wall_list;    // v3: layout walls (incl. hypothesised sides), outline order, at most kMaxWalls
  std::vector<OverlaySpan> span_list;    // v3: "spans" = "id,x0,z0,x1,z1,state;..." (always emitted), at most kMaxSpans
  uint32_t yaw_mask = 0;                 // bit i = a keyframe looked along heading [30i, 30i+30) deg (positive yaw = left)
  bool recalibrating = false;            // ADR 0031: tracking recovery active ("recalibrating":true, key absent when false)

  std::vector<OverlayObject> object_list;  // at most kMaxObjects; empty (and no JSON key) with understanding off
  // v3 (always emitted): the layout outline.
  std::vector<std::pair<double, double>> shell_corners;   // "shell" = "x,z;..." (outline corners in order; "" = none)
  bool shell_closed = false;                              // "shell_closed": closed outline AND every wall has evidence
  // Phase 9 step 8 (understanding >= 2 only; each key is absent while its list is empty):
  std::vector<OverlayOpening> opening_list;               // "openings" = "kind,wall,x0,z0,x1,z1,y0,y1;..."
  std::vector<OverlayGap> gap_list;                       // "gaps" = "type,x,z,pri;..."
  std::string gap_why_id, gap_why_text;                   // "gap_why_id" ("gap.<type>") and "gap_why_text" (landmark phrase) of gap #1
  static constexpr size_t kMaxOpenings = 6, kMaxCorners = 12, kMaxGaps = 4;
  static constexpr size_t kMaxObjects = 12;
  static constexpr size_t kMaxWalls = 12, kMaxSpans = 24;  // spans: limit_spans() shares the cap fairly per wall
  static constexpr int kMaxId = 999999;  // wall ids in surfaces/spans (fresh line numbers start at 100001)
  static constexpr int kYawSectors = 12;
  /// Flat JSON, sorted keys, fixed rounding. Walls are ONE delimited string "x0,z0,x1,z1,pct,id;..." (no nesting).
  /// Objects, only when present, likewise: key "objects" = "id,cls,x,z,w,d,yaw_deg,h;...".
  std::string to_json() const;
};

/// Core catalog (locale "en") for the "why" line. Every text is at most 8 words.
const char* why_line(const std::string& why_id);
/// All why ids, for tests.
std::vector<std::string> why_ids();

}  // namespace capturekit::guide
