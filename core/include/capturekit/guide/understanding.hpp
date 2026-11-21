#pragma once
// Phase 9 room-understanding value types (plain data, no behaviour): object classes, detections,
// lifted objects, openings, the room shell and gaps. Stages that fill them live in guide/*.cpp.
// World frame: Y up, metres. Pixel boxes are in the stored keyframe image, top-left origin.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "capturekit/guide/types.hpp"

namespace capturekit::guide {

/// Core object class ids (the shell maps detector labels to these once). 0 = unknown.
/// The ids are recorded in bundles (/ar/detections, /guide/labels): only APPEND before Count_, never renumber (ADR 0030).
/// 1..7 = the original COCO furniture set; 8..12 = further COCO indoor classes the shipped detector emits;
/// 13..16 = room classes COCO lacks, reserved for a detector trained on them (no shell maps to them yet).
/// Doors and windows are not object classes: they are wall openings from geometry (Opening, opening_detector).
enum class ObjectClass : int32_t {
  Unknown = 0, Chair, Couch, Bed, DiningTable, Tv, PottedPlant, Laptop,
  Bench, Toilet, Sink, Refrigerator, Oven,
  Desk, Shelf, Cabinet, Lamp,
  Count_
};

constexpr std::array<const char*, size_t(ObjectClass::Count_)> kObjectClassNames = {
    "unknown", "chair", "couch", "bed", "dining_table", "tv", "potted_plant", "laptop",
    "bench", "toilet", "sink", "refrigerator", "oven",
    "desk", "shelf", "cabinet", "lamp"};

inline const char* object_class_name(ObjectClass c) {
  size_t i = size_t(c);
  return i < kObjectClassNames.size() ? kObjectClassNames[i] : "unknown";
}
inline ObjectClass object_class_from_name(std::string_view s) {
  for (size_t i = 0; i < kObjectClassNames.size(); ++i)
    if (s == kObjectClassNames[i]) return ObjectClass(i);
  return ObjectClass::Unknown;
}
/// Wall-mounted classes never become floor obstacles.
inline bool object_class_wall_mounted(ObjectClass c) { return c == ObjectClass::Tv; }

/// A recorded 2D detection of one keyframe (input; replayed deterministically).
struct Detection {
  int64_t t_ns = 0;
  uint32_t frame_id = 0;
  ObjectClass cls = ObjectClass::Unknown;
  float score = 0;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

/// An object lifted to an oriented 3D box standing on the floor.
struct Object {
  uint32_t id = 0;
  ObjectClass cls = ObjectClass::Unknown;
  Vec3 centre = Vec3::Zero();  // box centre
  Vec3 size = Vec3::Zero();    // x = width, y = height, z = depth (box frame)
  double yaw = 0;              // rotation about +Y
  double conf = 0;
  int n_obs = 0;
};

enum class OpeningKind : int32_t { Door = 0, Window = 1 };

/// A door or window found in a wall (geometry only), in world metres.
struct Opening {
  OpeningKind kind = OpeningKind::Door;
  std::string wall;                     // stable wall id, e.g. "W1"; empty until the shell attaches it
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;  // footprint segment on the wall
  double y0 = 0, y1 = 0;                // bottom / top height above the floor
  double conf = 0;
};

/// A wall of the live room shell.
struct ShellWall {
  std::string id;                       // stable, "W1".."Wn"
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  bool inferred = false;                // hypothesised, not observed
};

struct ShellModel {
  std::vector<ShellWall> walls;
  std::vector<std::pair<double, double>> corners;  // (x, z), closed polygon when `closed`
  bool closed = false, low_confidence = false;
  double ceiling_h = 2.5;
  bool ceiling_inferred = true;
};

enum class GapType : int32_t { MissingWall = 0, UnseenCorner, UnseenCeiling, UnseenFloor, Occluded, Weak };

/// An expected-but-unobserved area (or area observed too weakly).
struct Gap {
  GapType type = GapType::MissingWall;
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  double pri = 0;                       // 0..1
  std::string why;                      // templated phrase, e.g. "behind the sofa"
};

}  // namespace capturekit::guide
