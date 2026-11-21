#pragma once
// Instruction generator (Phase 8, stage 5): a FIXED vocabulary rendered from templates (no LLM in the
// loop), egocentric wording, per-type cooldowns, a global minimum gap, no back-to-back repeats and
// priority interrupts (tracking loss / motion blur beat guidance). Every instruction has a lifecycle
// (issued -> completed | expired | superseded) that feeds the guide log.
// Yaw sign: positive = left. Parameters: MOVE_TO{distance_m, rel_bearing_deg}, ROTATE_TOWARD{rel_deg},
// TILT_UP/TILT_DOWN{deg}, STEP_BACK{meters}, SWEEP{dir, degrees}.
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "capturekit/guide/params.hpp"
#include "capturekit/guide/planner.hpp"

namespace capturekit::guide {

enum class InstrType : int {
  SlowDown = 0, HoldStill, Relocalize, MoveTo, RotateToward, TiltUp, TiltDown, StepBack, Sweep, MoreLight, Finish,
  Progress,     // one-shot "N % done" prompt (priority 1)
  KeepTurning,  // sweep progress prompt (priority 1)
  AimTextured,  // tracking limited by INSUFFICIENT_FEATURES: aim at a textured wall (priority 3)
  Recalibrate   // issue #1 / ADR 0031: tracking degraded > 2 s, turn the phone slowly in a small circle (priority 3)
};  // append-only: the integer values are type ids that shells and logs may store
const char* type_name(InstrType t);
int type_priority(InstrType t);  // 3 safety/tracking, 2 guidance, 1 info

enum class LifeState { Active, Completed, Expired, Superseded };
const char* life_name(LifeState s);

struct Instruction {
  uint32_t id = 0;
  InstrType type = InstrType::Finish;
  std::vector<std::pair<std::string, double>> params;
  int priority = 2;
  std::string msg_id, speak_text, long_text, haptic, overlay_hint;
  int64_t t_ns = 0;
  double param(const std::string& k, double def = 0) const {
    for (auto& [a, v] : params)
      if (a == k) return v;
    return def;
  }
};

/// Everything the generator needs about the current moment.
struct Situation {
  int64_t t_ns = 0;
  UserState user;
  int tracking_state = 1;     // 1 normal, 2 limited, 3 not available
  int tracking_reason = 0;    // bundle::TrackingReason code; only meaningful while tracking_state == 2
  int64_t lost_since_ns = 0;  // valid when tracking_state == 3
  int blur_streak = 0;        // consecutive frames rejected for motion blur
  bool underexposed = false;
  Plan plan;
  double too_close = 0;       // fraction of visible deficient cells nearer than range_min
  double completeness = 0;
  // Tracking recovery is active (recovery.hpp, ADR 0031): only RECALIBRATE (or RELOCALIZE after a long loss) is spoken,
  // active guidance is superseded and no navigation, info or quality prompt is issued.
  bool recovering = false;
};

struct LifeEvent {
  Instruction ins;
  LifeState state;
  int64_t t_end_ns;
  double completeness_end;
};

class InstructionGenerator {
 public:
  explicit InstructionGenerator(const GuideParams& p) : p_(p) {}
  /// New instructions are appended to `out`; lifecycle changes (complete/expire/supersede) to `events`.
  void update(const Situation& s, std::vector<Instruction>& out, std::vector<LifeEvent>& events);
  /// Catalog lookup (locale "en"): short spoken variant and long text for a message id. `variant` picks one of
  /// variant_count(msg_id) phrasings (modulo). {m_unit} is derived from vars["m"] ("1" -> "meter", else "meters").
  static std::string render(const std::string& msg_id, const std::map<std::string, std::string>& vars, bool longer,
                            uint32_t variant = 0);
  static int variant_count(const std::string& msg_id);

 private:
  struct Active {
    Instruction ins;
    double yaw0 = 0, pitch0 = 0, cum_yaw = 0, last_yaw = 0;
    Vec3 pos0;
    int64_t calm_since = -1;
    int turn_prompts = 0;  // "Keep turning" prompts already given for this Sweep
  };
  bool cooled(InstrType t, int64_t now) const;
  Instruction make(InstrType t, const Situation& s, std::vector<std::pair<std::string, double>> params);
  void finish_active(Active& a, LifeState st, const Situation& s, std::vector<LifeEvent>& ev);
  bool complete(Active& a, const Situation& s) const;
  static int bucket(const Instruction& i);

  GuideParams p_;
  uint32_t next_id_ = 1;
  std::vector<Active> active_;  // at most one guidance + one urgent/info
  std::map<int, int64_t> last_issue_;  // per type
  int64_t last_guidance_ns_ = -1, start_ns_ = -1, last_info_ns_ = -1;
  int progress_next_ = 0;  // index of the next 25/50/75 % prompt
  static constexpr double kInfoGapS = 6;  // min gap between progress / keep-turning prompts
};

}  // namespace capturekit::guide
