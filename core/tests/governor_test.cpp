// Phase 5 (tests first): table-driven scripted timelines for the thermal/battery governor.
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "capturekit/governor/governor.hpp"

using namespace capturekit::governor;

namespace {
constexpr int64_t kS = 1'000'000'000;

Signals cool(int64_t t) {
  Signals s;
  s.t_ns = t;
  s.thermal_level = 0;
  s.battery_pct = 80;
  s.charging = false;
  s.frame_time_ms_ema = 20;
  s.dropped_frame_rate = 0;
  return s;
}

struct Run {
  std::vector<Policy> policies;
  std::vector<Transition> transitions;
};

// Steps a timeline every 250 ms from 0 to `seconds`, signals shaped by `shape(t_seconds, s)`.
Run run(double seconds, const std::function<void(double, Signals&)>& shape, GovernorConfig cfg = {}) {
  Governor g(cfg);
  Run r;
  for (int64_t t = 0; t <= int64_t(seconds * 4); ++t) {
    Signals s = cool(t * kS / 4);
    shape(double(t) / 4, s);
    r.policies.push_back(g.update(s, r.transitions));
  }
  return r;
}

int max_level(const Run& r) {
  int m = 0;
  for (auto& p : r.policies) m = std::max(m, int(p.level));
  return m;
}
}  // namespace

struct TimelineCase {
  const char* name;
  double seconds;
  std::function<void(double, Signals&)> shape;
  std::function<void(const Run&)> check;
};

class GovernorTimeline : public ::testing::TestWithParam<TimelineCase> {};

TEST_P(GovernorTimeline, Script) {
  auto& c = GetParam();
  c.check(run(c.seconds, c.shape));
}

INSTANTIATE_TEST_SUITE_P(
    Governor, GovernorTimeline,
    ::testing::Values(
        TimelineCase{"nominal_stays_nominal", 60, [](double, Signals&) {},
                     [](const Run& r) {
                       EXPECT_TRUE(r.transitions.empty());
                       auto& p = r.policies.back();
                       EXPECT_EQ(p.level, Level::Nominal);
                       EXPECT_DOUBLE_EQ(p.keyframe_rate_scale, 1.0);
                       EXPECT_TRUE(p.upload_allowed && p.overlay_enabled && !p.must_pause);
                     }},
        TimelineCase{"slow_heat_up_climbs_ladder_in_order", 240,
                     [](double t, Signals& s) { s.thermal_level = t < 60 ? 0 : t < 120 ? 1 : t < 180 ? 2 : 3; },
                     [](const Run& r) {
                       std::vector<int> to;
                       for (auto& x : r.transitions) to.push_back(int(x.to));
                       EXPECT_EQ(to, (std::vector<int>{int(Level::ReduceKeyframes), int(Level::LowerResolution),
                                                       int(Level::SafePause)}));
                       EXPECT_TRUE(r.policies.back().must_pause);
                       for (size_t i = 1; i < r.policies.size(); ++i)
                         EXPECT_GE(int(r.policies[i].level), int(r.policies[i - 1].level));  // monotone up
                     }},
        TimelineCase{"oscillation_at_boundary_does_not_flap", 120,
                     [](double t, Signals& s) { s.thermal_level = (int(t) % 2 == 0) ? 1 : 2; },
                     [](const Run& r) { EXPECT_LE(r.transitions.size(), 2u); }},
        TimelineCase{"sudden_critical_pauses_immediately", 20,
                     [](double t, Signals& s) { s.thermal_level = t >= 5 ? 3 : 0; },
                     [](const Run& r) {
                       ASSERT_EQ(r.transitions.size(), 1u);
                       EXPECT_EQ(r.transitions[0].to, Level::SafePause);
                       EXPECT_EQ(r.transitions[0].t_ns, 5 * kS);
                       EXPECT_NE(r.transitions[0].reason.find("thermal"), std::string::npos);
                     }},
        TimelineCase{"recovery_steps_down_one_level_per_dwell", 120,
                     [](double t, Signals& s) { s.thermal_level = t < 10 ? 2 : 0; },
                     [](const Run& r) {
                       EXPECT_EQ(r.policies.back().level, Level::Nominal);
                       // Up to LowerResolution, then 3 single steps down, each >= min dwell apart.
                       ASSERT_EQ(r.transitions.size(), 4u);
                       for (size_t i = 2; i < r.transitions.size(); ++i) {
                         EXPECT_EQ(int(r.transitions[i].to), int(r.transitions[i - 1].to) - 1);
                         EXPECT_GE(r.transitions[i].t_ns - r.transitions[i - 1].t_ns, 10 * kS);
                       }
                     }},
        TimelineCase{"low_battery_not_charging_warns_then_pauses", 60,
                     [](double t, Signals& s) { s.battery_pct = 20 - t / 3; },
                     [](const Run& r) {
                       bool warned = false;
                       for (auto& x : r.transitions) warned |= x.to == Level::Warn;
                       EXPECT_TRUE(warned);
                       EXPECT_TRUE(r.policies.back().must_pause);
                       EXPECT_FALSE(r.policies.back().upload_allowed);
                     }},
        TimelineCase{"low_battery_charging_is_fine", 60,
                     [](double t, Signals& s) {
                       s.battery_pct = 20 - t / 3;
                       s.charging = true;
                     },
                     [](const Run& r) {
                       EXPECT_TRUE(r.transitions.empty());
                       EXPECT_TRUE(r.policies.back().upload_allowed);
                     }},
        TimelineCase{"short_drop_spike_on_cool_device_ignored", 30,
                     [](double t, Signals& s) {
                       if (t >= 10 && t < 11) s.dropped_frame_rate = 0.5;
                     },
                     [](const Run& r) { EXPECT_TRUE(r.transitions.empty()); }},
        TimelineCase{"sustained_drops_degrade_mildly_never_pause", 60,
                     [](double t, Signals& s) {
                       if (t >= 10) s.dropped_frame_rate = 0.5, s.frame_time_ms_ema = 60;
                     },
                     [](const Run& r) {
                       EXPECT_EQ(max_level(r), int(Level::ReduceMl));
                       EXPECT_FALSE(r.policies.back().must_pause);
                     }},
        TimelineCase{"low_power_mode_reduces_keyframes", 10,
                     [](double, Signals& s) { s.low_power_mode = true; },
                     [](const Run& r) {
                       EXPECT_EQ(r.policies.back().level, Level::ReduceKeyframes);
                       EXPECT_LT(r.policies.back().keyframe_rate_scale, 1.0);
                     }}),
    [](const ::testing::TestParamInfo<TimelineCase>& i) { return std::string(i.param.name); });

TEST(Governor, LadderPoliciesAreMonotone) {
  GovernorConfig cfg;
  Policy prev = policy_for(Level::Nominal, cfg);
  for (int l = 1; l <= int(Level::SafePause); ++l) {
    Policy p = policy_for(Level(l), cfg);
    EXPECT_LE(p.keyframe_rate_scale, prev.keyframe_rate_scale);
    EXPECT_LE(p.ml_rate_scale, prev.ml_rate_scale);
    EXPECT_GE(p.capture_resolution_tier, prev.capture_resolution_tier);
    prev = p;
  }
  EXPECT_TRUE(policy_for(Level::SafePause, cfg).must_pause);
  EXPECT_FALSE(policy_for(Level::LowerResolution, cfg).overlay_enabled);
}

TEST(Governor, ConfigOverridesAndUnknownKey) {
  auto c = GovernorConfig::from_json("{\"down_dwell_s\": 3}");
  ASSERT_TRUE(c);
  EXPECT_DOUBLE_EQ(c->down_dwell_s, 3);
  EXPECT_FALSE(GovernorConfig::from_json("{\"nope\": 1}"));
}

TEST(Governor, ThermalStateNames) {
  EXPECT_EQ(thermal_level_from_name("nominal"), 0);
  EXPECT_EQ(thermal_level_from_name("serious"), 2);
  EXPECT_EQ(thermal_level_from_name("critical"), 3);
  EXPECT_EQ(thermal_level_from_name("bogus"), -1);
}

#include "capturekit/governor/governor_sink.hpp"

TEST(GovernorSinkTest, InjectedThermalChangesPolicy) {
  GovernorSink s{GovernorConfig{}};
  auto ev = [](int64_t t, const char* code, std::vector<std::pair<std::string, std::string>> a) {
    capturekit::bundle::EventRecord e;
    e.t_ns = t;
    e.source = "platform";
    e.code = code;
    e.attrs = std::move(a);
    return e;
  };
  s.on_event(ev(0, "thermal", {{"state", "nominal"}}));
  s.on_event(ev(5 * kS, "thermal", {{"state", "serious"}}));
  s.on_event(ev(8 * kS, "thermal", {{"state", "nominal"}}));
  for (int64_t t = 9; t <= 60; ++t) {
    capturekit::replay::FrameView f;
    f.t_ns = t * kS;
    s.on_frame(f);
  }
  s.on_end(60 * kS);
  ASSERT_GE(s.transitions().size(), 2u);
  EXPECT_EQ(s.transitions()[0].to, Level::LowerResolution);
  EXPECT_EQ(s.transitions().back().to, Level::Nominal);
  auto j = s.report_json();
  EXPECT_NE(j.find("\"max_level\":\"LOWER_RESOLUTION\""), std::string::npos) << j;
}
