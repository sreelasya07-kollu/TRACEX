"""`ckit guide-eval`: does guidance help? Simulated walkers on synthetic rooms (the C++ GuideMind
engine runs for real), and guidance replay on recorded bundles.

Simulator mode: rooms x policies x seeds. Policies: `planner` (follows instructions with a human
model: compliance, 1-3 s reaction delay, yaw noise), `scripted` (looped perimeter laps + centre
spins) and `wander` (random walk). Ground-truth coverage uses the engine's sufficiency rule
with true occlusion; time-to-coverage that is never reached is censored at the horizon.
"""

from __future__ import annotations

import json
import statistics
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from ckit.core import load

ROOMS = ("box", "L", "furniture")
# ADR 0029: furnished rooms with ARCore-like wall planes (fragments, re-ids, subsumption, jitter).
FURNISHED_ROOMS = ("furnished4", "furnishedL")
POLICIES = ("planner", "scripted", "wander")
KEEP = (
    "duration_s", "path_m", "finished", "coverage_gt_final", "coverage_claimed_final", "t80_s",
    "t90_s",
    "t95_s", "finish_issued_s", "flap_per_min", "unsafe_targets_gt", "tracking_loss_events",
    "blocked_events", "instructions_received", "instructions_followed", "keyframes", "params_hash",
)  # fmt: skip
# Spoken-prompt budget: mirrors the Android SpeechScheduler.Config.maxPerMinute default
# (non-urgent lines per minute).
SPEECH_BUDGET_PER_MIN = 8.0
# Real-room fix: core-side rolling cap on spoken guidance (GuideParams max_prompts_per_min). Counts
# every instruction except the safety/tracking ones (priority 3 in core instruction.cpp).
PROMPT_CAP_PER_MIN = 6.0
SAFETY_TYPES = frozenset({"SLOW_DOWN", "HOLD_STILL", "RELOCALIZE", "AIM_TEXTURED", "RECALIBRATE"})
# Completeness may dip (re-ids, layout merges) but by at most this much below its running maximum.
TIMELINE_MAX_DIP = 0.05


def prompt_demand_per_min(by_type: dict | None, duration_s) -> float | None:
    """Non-safety instructions per minute (what the 6/min cap limits). None without data."""
    if by_type is None or not duration_s or duration_s <= 0:
        return None
    n = sum(v for k, v in by_type.items() if k not in SAFETY_TYPES)
    return round(n * 60.0 / duration_s, 2)


def timeline_stats(points) -> dict | None:
    """Completeness timeline [(t_s, completeness), ...] -> final value, largest dip below the
    running maximum, and whether every dip is within TIMELINE_MAX_DIP. None when empty."""
    pts = [(float(t), float(c)) for t, c in (points or []) if c is not None]
    if not pts:
        return None
    peak, dip = pts[0][1], 0.0
    for _, c in pts:
        peak = max(peak, c)
        dip = max(dip, peak - c)
    return {
        "n": len(pts),
        "t_end_s": round(pts[-1][0], 3),
        "final": round(pts[-1][1], 3),
        "max_dip": round(dip, 3),
        "within_dip": dip <= TIMELINE_MAX_DIP + 1e-9,
    }


def layout_info(guide_report: dict | None) -> tuple[dict | None, list | None]:
    """(layout, timeline) from an engine report. Both keys come from the T1 layout contract
    (`layout{walls,closed,blocked_m}`, `timeline[{t_s,completeness,walls,prompts}]`); None when the
    engine does not report them yet."""
    g = guide_report or {}
    lay, tl = g.get("layout"), g.get("timeline")
    return (lay if isinstance(lay, dict) else None), (tl if isinstance(tl, list) else None)


def _timeline_points(timeline: list | None) -> list:
    return [(e.get("t_s"), e.get("completeness")) for e in timeline or [] if isinstance(e, dict)]


def max_prompts_in_window(timeline: list | None, window_s: float = 60.0) -> int | None:
    """Most instructions issued within any `window_s` span of the engine timeline (its `prompts` is
    cumulative over all types, safety included, sampled every 5 s since the first pose: windows
    (t_j, t_i] with t_i - t_j <= window_s, plus (0, t_i] while t_i <= window_s)."""
    pts = [
        (float(e["t_s"]), int(e["prompts"]))
        for e in timeline or []
        if isinstance(e, dict) and e.get("t_s") is not None and e.get("prompts") is not None
    ]
    if not pts:
        return None
    best, j = 0, 0
    for t, p in pts:
        while t - pts[j][0] > window_s:
            j += 1
        best = max(best, p - pts[j][1], p if t <= window_s else 0)
    return best


def instruction_demand_per_min(instructions, duration_s) -> float | None:
    """Instruction demand/min: engine instructions (all priorities) per minute, counted BEFORE the
    Android speech scheduler drops anything (gap, budget, repeat, busy). An upper bound on spoken
    prompts/min, not the spoken rate. None when duration is unknown or zero."""
    if instructions is None or not duration_s or duration_s <= 0:
        return None
    return round(instructions * 60.0 / duration_s, 2)


def _one(args: tuple) -> dict:
    room, policy, seed, max_s, params, compliance, speed = args
    r = json.loads(load().guide.simulate(room, policy, seed, max_s, params, compliance, speed))
    out = {k: r.get(k) for k in KEEP}
    out.update(room=room, policy=policy, seed=seed)
    g = r.get("guide")
    if g:
        out["repetition_rate"] = g["log"]["repetition_rate"]
        out["instructions"] = g["log"]["instructions"]
        out["unsafe_targets_engine"] = g["log"]["unsafe_targets"]
        out["by_type"] = g["log"]["by_type"]
    out["instruction_demand_per_min"] = instruction_demand_per_min(
        out.get("instructions"), out.get("duration_s")
    )
    out.update(_layout_fields(g, out.get("duration_s")))
    out["coverage_gt_timeline"] = timeline_stats(r.get("coverage_series_10s"))
    if r.get("sim_planes") is not None:
        out["sim_planes"] = r["sim_planes"]
    return out


def _layout_fields(g: dict | None, duration_s) -> dict:
    """Layout walls, engine completeness timeline stats and prompt demand (shared by both modes)."""
    lay, tl = layout_info(g)
    by_type = g["log"].get("by_type") if g else None
    return {
        "layout_walls": lay.get("walls") if lay else None,
        "layout_closed": lay.get("closed") if lay else None,
        "layout_blocked_m": lay.get("blocked_m") if lay else None,
        "completeness_timeline": timeline_stats(_timeline_points(tl)),
        "timeline_max_prompts_60s": max_prompts_in_window(tl),
        "prompt_demand_per_min": prompt_demand_per_min(by_type, duration_s),
    }


def _median_or_none(xs) -> float | None:
    v = [x for x in xs if x is not None]
    return round(statistics.median(v), 3) if v else None


def _censored(v, horizon: float) -> float:
    return horizon + 1.0 if v is None or v < 0 else float(v)


def simulate_matrix(
    rooms=ROOMS, policies=POLICIES, seeds=3, max_s=450.0, params: dict | None = None,
    compliance=0.85, speed=0.9, workers=6,
) -> dict:  # fmt: skip
    p = json.dumps(params or {}, sort_keys=True)
    jobs = [
        (r, pol, s, max_s, p, compliance, speed)
        for r in rooms
        for pol in policies
        for s in range(seeds)
    ]
    with ProcessPoolExecutor(max_workers=workers) as ex:
        runs = list(ex.map(_one, jobs))
    runs.sort(key=lambda r: (r["room"], r["policy"], r["seed"]))
    summary = summarize(runs, rooms, policies, max_s)
    return {
        "config": {"rooms": list(rooms), "policies": list(policies), "seeds": seeds, "max_s": max_s,
                   "params": params or {}, "compliance": compliance, "speed": speed},
        "summary": summary, "acceptance": acceptance(runs, summary, rooms), "runs": runs,
    }  # fmt: skip


def summarize(runs: list[dict], rooms, policies, max_s: float) -> dict:
    """Per room x policy medians (pure)."""
    summary: dict = {}
    for r in rooms:
        for pol in policies:
            g = [x for x in runs if x["room"] == r and x["policy"] == pol]
            summary.setdefault(r, {})[pol] = {
                "median_final_coverage": round(
                    statistics.median(x["coverage_gt_final"] for x in g), 3
                ),
                "median_t80_s": statistics.median(_censored(x["t80_s"], max_s) for x in g),
                "median_t90_s": statistics.median(_censored(x["t90_s"], max_s) for x in g),
                "median_t90_censored": bool(g)
                and statistics.median(_censored(x["t90_s"], max_s) for x in g) > max_s,
                "reached_90": sum(1 for x in g if x["t90_s"] is not None and x["t90_s"] >= 0),
                "median_instruction_demand_per_min": _median_or_none(
                    x.get("instruction_demand_per_min") for x in g
                ),
                "median_repetition_rate": _median_or_none(x.get("repetition_rate") for x in g),
                "median_prompt_demand_per_min": _median_or_none(
                    x.get("prompt_demand_per_min") for x in g
                ),
                "median_layout_walls": _median_or_none(x.get("layout_walls") for x in g),
                "runs": len(g),
            }
    return summary


def _planner_beats_wander_t90(room_summary: dict) -> bool | None:
    """Planner median t90 below wander's; None when neither reached t90 (both censored at max_s)."""
    p, w = room_summary["planner"], room_summary["wander"]
    if p.get("median_t90_censored") and w.get("median_t90_censored"):
        return None
    return p["median_t90_s"] < w["median_t90_s"]


def acceptance(runs: list[dict], summary: dict, rooms) -> dict:
    """Acceptance block over planner runs (pure). Budget verdict is None without data."""
    pl = [x for x in runs if x["policy"] == "planner"]
    gap = max(
        (abs((x["coverage_claimed_final"] or 0) - x["coverage_gt_final"]) for x in pl), default=0
    )
    rep_rate = statistics.median([x.get("repetition_rate", 0) for x in pl] or [0])
    ppm = [
        x["instruction_demand_per_min"]
        for x in pl
        if x.get("instruction_demand_per_min") is not None
    ]
    prompts = [x["prompt_demand_per_min"] for x in pl if x.get("prompt_demand_per_min") is not None]
    dips = [x["completeness_timeline"]["max_dip"] for x in pl if x.get("completeness_timeline")]
    vs_wander = {
        r: _planner_beats_wander_t90(summary[r])
        for r in rooms if "wander" in summary[r] and "planner" in summary[r]
    }  # fmt: skip
    neither = [r for r, v in vs_wander.items() if v is None]
    if any(v is False for v in vs_wander.values()):
        beats_all = False
    elif not vs_wander or neither:
        beats_all = None  # unknown: no wander runs, or neither policy reached t90 in some room
    else:
        beats_all = True
    return {
        "planner_beats_scripted_median_t90_rooms": sum(
            1 for r in rooms if "scripted" in summary[r] and "planner" in summary[r]
            and summary[r]["planner"]["median_t90_s"] < summary[r]["scripted"]["median_t90_s"]
        ),
        "planner_beats_wander_median_t90_rooms": sum(
            1 for r in rooms if "wander" in summary[r] and "planner" in summary[r]
            and summary[r]["planner"]["median_t90_s"] < summary[r]["wander"]["median_t90_s"]
        ),
        "rooms": len(rooms),
        "unsafe_targets_gt_total": sum(x["unsafe_targets_gt"] or 0 for x in pl),
        "max_flap_per_min": max((x["flap_per_min"] or 0 for x in pl), default=0),
        "max_claim_vs_truth_gap": round(gap, 3),
        "median_repetition_rate": round(rep_rate, 3),
        "median_instruction_demand_per_min": round(statistics.median(ppm), 2) if ppm else None,
        "max_instruction_demand_per_min": max(ppm) if ppm else None,
        "prompts_per_min_budget": SPEECH_BUDGET_PER_MIN,
        "instruction_demand_within_budget": (
            all(v <= SPEECH_BUDGET_PER_MIN for v in ppm) if ppm else None
        ),
        "planner_beats_wander_all_rooms": beats_all,
        "neither_reached_t90_rooms": neither,
        "prompt_cap_per_min": PROMPT_CAP_PER_MIN,
        "max_prompt_demand_per_min": max(prompts) if prompts else None,
        "prompts_within_cap": all(v <= PROMPT_CAP_PER_MIN for v in prompts) if prompts else None,
        "layout_walls_reported": any(x.get("layout_walls") is not None for x in pl),
        "max_completeness_dip": max(dips) if dips else None,
    }  # fmt: skip


def acceptance_failures(acc: dict) -> list[str]:
    """The real-room acceptance checks that failed (pure; unknown = not failed, see the report)."""
    fails = []
    if acc.get("planner_beats_wander_all_rooms") is False:
        fails.append("planner median t90 not below wander in every room")
    if acc.get("prompts_within_cap") is False:
        fails.append(f"prompt demand {acc['max_prompt_demand_per_min']}/min > {PROMPT_CAP_PER_MIN}")
    if acc.get("unsafe_targets_gt_total"):
        fails.append(f"{acc['unsafe_targets_gt_total']} unsafe targets")
    dip = acc.get("max_completeness_dip")
    if dip is not None and dip > TIMELINE_MAX_DIP + 1e-9:
        fails.append(f"completeness dipped {dip} > {TIMELINE_MAX_DIP}")
    return fails


def replay_bundle(bundle: Path, params: dict | None = None) -> dict:
    """Guidance engine on a recorded bundle: final coverage, instruction count, repetition rate,
    plan-through-obstacle (unsafe target) violations."""
    from ckit.replay.driver import load_events, replay

    rep = replay(bundle, ["guide"], sink_config={"guide": params or {}})
    g = rep["sinks"]["guide"]["engine"]
    ts = [e[0] for e in load_events(bundle)[0]]
    dur = (max(ts) - min(ts)) / 1e9 if ts else 0.0
    lay, tl = layout_info(g)
    fields = _layout_fields(g, dur)
    return {
        **fields,
        "layout": lay,
        "timeline": tl,
        "prompts_within_cap": (
            None
            if fields["prompt_demand_per_min"] is None
            else fields["prompt_demand_per_min"] <= PROMPT_CAP_PER_MIN
        ),
        "bundle": bundle.name,
        "tier": g["tier"],
        "coverage_completeness": g["coverage"]["completeness"],
        "coverage_seen": g["coverage"]["seen"],
        "planes": g["coverage"]["planes"],
        "instructions": g["log"]["instructions"],
        "by_type": g["log"]["by_type"],
        "repetition_rate": g["log"]["repetition_rate"],
        "duration_s": round(dur, 3),
        "instruction_demand_per_min": instruction_demand_per_min(g["log"]["instructions"], dur),
        "unsafe_targets": g["log"]["unsafe_targets"],
        "target_switches": g["log"]["target_switches"],
        "first_instructions": rep["sinks"]["guide"]["first_instructions"][:5],
    }


def table(rep: dict) -> str:
    lines = [
        "room       policy    cov@end  t80    t90    reached90  walls  prompts/min  demand/min   "
        "repetition"
    ]
    for room, d in rep["summary"].items():
        for pol, s in d.items():
            reached = f"{s['reached_90']}/{s['runs']}"
            ppm = s.get("median_instruction_demand_per_min")
            rep_rate = s.get("median_repetition_rate")
            walls, prompts = s.get("median_layout_walls"), s.get("median_prompt_demand_per_min")
            lines.append(
                f"{room:<10} {pol:<9} {s['median_final_coverage']:<8} {s['median_t80_s']:<6.0f} "
                f"{s['median_t90_s']:<6.0f} {reached:<10} {_fmt(walls):<6} {_fmt(prompts):<12} "
                f"{_fmt(ppm):<12} {_fmt(rep_rate)}"
            )
    lines.append(json.dumps(rep["acceptance"], sort_keys=True))
    return "\n".join(lines)


def _fmt(v) -> str:
    return "-" if v is None else f"{v:g}"
