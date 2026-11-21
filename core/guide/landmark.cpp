#include "capturekit/guide/landmark.hpp"

#include <algorithm>
#include <cmath>

#include "capturekit/guide/types.hpp"

namespace capturekit::guide {

namespace {
double dist_seg(double px, double pz, double ax, double az, double bx, double bz) {
  const double dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
  const double t = l2 > 0 ? std::clamp(((px - ax) * dx + (pz - az) * dz) / l2, 0.0, 1.0) : 0.0;
  return std::hypot(px - (ax + t * dx), pz - (az + t * dz));
}
}  // namespace

const char* LandmarkLabeler::class_phrase(ObjectClass c) {
  switch (c) {
    case ObjectClass::Chair: return "chair";
    case ObjectClass::Couch: return "sofa";
    case ObjectClass::Bed: return "bed";
    case ObjectClass::DiningTable: return "table";
    case ObjectClass::Tv: return "TV";
    case ObjectClass::PottedPlant: return "plant";
    case ObjectClass::Laptop: return "laptop";
    case ObjectClass::Bench: return "bench";
    case ObjectClass::Toilet: return "toilet";
    case ObjectClass::Sink: return "sink";
    case ObjectClass::Refrigerator: return "fridge";
    case ObjectClass::Oven: return "oven";
    case ObjectClass::Desk: return "desk";
    case ObjectClass::Shelf: return "shelf";
    case ObjectClass::Cabinet: return "cabinet";
    case ObjectClass::Lamp: return "lamp";
    default: return "furniture";
  }
}

std::string LandmarkLabeler::direction(const LandmarkContext& ctx, double x, double z) {
  if (!ctx.has_user) return "";
  const double rel = wrap_pi(std::atan2(x - ctx.ux, z - ctx.uz) - ctx.yaw);  // + = to the left
  const double a = std::fabs(rel);
  if (a < kPi / 4) return "ahead";
  if (a > 3 * kPi / 4) return "behind you";
  return rel > 0 ? "on your left" : "on your right";
}

std::string LandmarkLabeler::label(const Gap& g, const std::vector<Opening>& openings,
                                   const std::vector<Object>& objects, const LandmarkContext& ctx) const {
  const double cx = 0.5 * (g.x0 + g.x1), cz = 0.5 * (g.z0 + g.z1);
  const std::string dir = direction(ctx, cx, cz);
  // The nearest opening to the gap centre names a wall / corner gap ("window wall" before "door wall").
  const Opening* op = nullptr;
  double best = opening_near_m;
  for (const auto& o : openings) {
    const double d = dist_seg(cx, cz, o.x0, o.z0, o.x1, o.z1);
    if (d < best) best = d, op = &o;
  }
  const char* op_name = op ? (op->kind == OpeningKind::Window ? "window" : "door") : "";
  switch (g.type) {
    case GapType::MissingWall:
    case GapType::Weak:
      if (op) return std::string("the ") + op_name + " wall";
      return dir.empty() ? "an unseen wall" : "the wall " + dir;
    case GapType::UnseenCorner:
      if (op) return std::string("the corner near the ") + op_name;
      return dir.empty() ? "an unseen corner" : "the corner " + dir;
    case GapType::UnseenCeiling: return "the ceiling";
    case GapType::UnseenFloor: return "the floor";
    case GapType::Occluded: {
      const Object* ob = nullptr;
      double bd = object_near_m;
      for (const auto& o : objects) {
        const double d = std::hypot(o.centre.x() - cx, o.centre.z() - cz);
        if (d < bd) bd = d, ob = &o;
      }
      if (!ob) return "a hidden spot";
      const std::string nm = class_phrase(ob->cls);
      if (!ctx.has_user) return "around the " + nm;
      // Far side of the object seen from the user = "behind"; otherwise "beside".
      const double fx = ob->centre.x() - ctx.ux, fz = ob->centre.z() - ctx.uz;
      const double dot = (cx - ob->centre.x()) * fx + (cz - ob->centre.z()) * fz;
      return std::string(dot > 0 ? "behind" : "beside") + " the " + nm;
    }
  }
  return "an unseen area";
}

}  // namespace capturekit::guide
