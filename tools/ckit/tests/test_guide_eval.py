"""Real-room reporting in `ckit guide-eval`: layout walls, completeness timeline, prompts/min vs the
6/min cap, planner-vs-wander verdict, and the furnished arcore-plane rooms (ADR 0029)."""

import json

from ckit.guide_eval import (
    FURNISHED_ROOMS,
    PROMPT_CAP_PER_MIN,
    _one,
    acceptance,
    acceptance_failures,
    layout_info,
    max_prompts_in_window,
    prompt_demand_per_min,
    summarize,
    table,
    timeline_stats,
)


def test_prompt_demand_excludes_safety_prompts():
    by_type = {"MOVE_TO": 4, "PROGRESS": 2, "SLOW_DOWN": 5, "RELOCALIZE": 1, "AIM_TEXTURED": 3}
    assert prompt_demand_per_min(by_type, 60.0) == 6.0
    assert prompt_demand_per_min({"HOLD_STILL": 9}, 30.0) == 0.0
    assert prompt_demand_per_min(None, 60.0) is None
    assert prompt_demand_per_min(by_type, 0.0) is None
    assert PROMPT_CAP_PER_MIN == 6.0


def test_timeline_stats_dips_against_running_max():
    s = timeline_stats([(0, 0.1), (10, 0.4), (20, 0.37), (30, 0.5), (40, 0.6)])
    assert s == {"n": 5, "t_end_s": 40.0, "final": 0.6, "max_dip": 0.03, "within_dip": True}
    s = timeline_stats([[0, 0.2], [10, 0.5], [20, 0.4], [30, 0.45]])
    assert s["max_dip"] == 0.1 and s["within_dip"] is False and s["final"] == 0.45
    assert timeline_stats([]) is None
    assert timeline_stats(None) is None


def test_max_prompts_in_window_from_cumulative_timeline():
    tl = [{"t_s": t, "prompts": p} for t, p in [(0, 1), (30, 3), (60, 5), (65, 9), (130, 12)]]
    assert max_prompts_in_window(tl) == 6  # (30 s, 65 s] = 9 - 3 beats the first minute (5)
    assert max_prompts_in_window(tl[:3]) == 5  # first minute counts from the start
    assert max_prompts_in_window([{"t_s": 5, "completeness": 0.1}]) is None
    assert max_prompts_in_window(None) is None


def test_layout_info_reads_contract_keys_and_falls_back():
    g = {
        "layout": {"walls": 4, "closed": True, "blocked_m": 1.5},
        "timeline": [{"t_s": 1.0, "completeness": 0.1, "walls": 2, "prompts": 1}],
    }
    assert layout_info(g) == (g["layout"], g["timeline"])
    assert layout_info({"log": {}}) == (None, None)  # engine without the layout model yet
    assert layout_info(None) == (None, None)


def _run(room, policy, seed, t90, prompts=None, walls=None, dip=None):
    return {
        "room": room, "policy": policy, "seed": seed, "coverage_gt_final": 0.9,
        "coverage_claimed_final": 0.9, "t80_s": 50.0, "t90_s": t90, "flap_per_min": 0.5,
        "unsafe_targets_gt": 0, "repetition_rate": 0.1, "instruction_demand_per_min": prompts,
        "prompt_demand_per_min": prompts, "layout_walls": walls,
        "completeness_timeline": None if dip is None else {"max_dip": dip},
    }  # fmt: skip


def test_acceptance_reports_layout_prompts_and_planner_vs_wander():
    rooms = ("box", "furnished4")
    runs = [
        _run("box", "planner", 0, 100.0, prompts=4.0, walls=4, dip=0.02),
        _run("box", "wander", 0, 200.0),
        _run("furnished4", "planner", 0, 150.0, prompts=5.5, walls=5, dip=0.04),
        _run("furnished4", "wander", 0, -1),  # never reached 90 %: censored at the horizon
    ]
    summary = summarize(runs, rooms, ("planner", "wander"), 300.0)
    acc = acceptance(runs, summary, rooms)
    assert acc["planner_beats_wander_all_rooms"] is True
    assert acc["prompts_within_cap"] is True and acc["max_prompt_demand_per_min"] == 5.5
    assert acc["layout_walls_reported"] is True and acc["max_completeness_dip"] == 0.04
    assert summary["furnished4"]["planner"]["median_layout_walls"] == 5
    assert acceptance_failures(acc) == []
    lines = table({"summary": summary, "acceptance": acc}).splitlines()
    assert "walls" in lines[0] and "prompts/min" in lines[0]
    assert lines[1].split()[6:8] == ["4", "4"]  # walls, prompts/min
    assert json.loads(lines[-1])["planner_beats_wander_all_rooms"] is True

    runs[0]["t90_s"], runs[2]["prompt_demand_per_min"] = 250.0, 7.0
    runs[2]["completeness_timeline"] = {"max_dip": 0.2}
    summary = summarize(runs, rooms, ("planner", "wander"), 300.0)
    acc = acceptance(runs, summary, rooms)
    assert acc["planner_beats_wander_all_rooms"] is False
    assert acc["prompts_within_cap"] is False
    fails = acceptance_failures(acc)
    assert len(fails) == 3 and "wander" in fails[0] and "7.0" in fails[1] and "0.2" in fails[2]


def test_acceptance_without_wander_or_layout_is_unknown():
    runs = [_run("box", "planner", 0, 100.0)]
    acc = acceptance(runs, summarize(runs, ("box",), ("planner",), 300.0), ("box",))
    assert acc["planner_beats_wander_all_rooms"] is None
    assert acc["prompts_within_cap"] is None and acc["max_prompt_demand_per_min"] is None
    assert acc["layout_walls_reported"] is False and acc["max_completeness_dip"] is None
    assert acceptance_failures(acc) == []


def test_furnished_room_run_reports_arcore_planes_and_timelines():
    assert FURNISHED_ROOMS == ("furnished4", "furnishedL")
    out = _one(("furnished4", "planner", 0, 30.0, "{}", 0.85, 0.9))
    sp = out["sim_planes"]
    assert sp["mode"] == "arcore" and sp["true_walls"] == 4 and sp["fragments"] == 6
    assert out["coverage_gt_timeline"]["n"] >= 1
    assert out["prompt_demand_per_min"] is not None
    assert out["prompt_demand_per_min"] <= out["instruction_demand_per_min"]
    # layout keys are optional until the engine reports them (T1 contract): present -> int
    assert out["layout_walls"] is None or isinstance(out["layout_walls"], int)
    scripted = _one(("furnishedL", "scripted", 0, 20.0, "{}", 0.85, 0.9))
    assert scripted["sim_planes"]["true_walls"] == 6 and scripted["layout_walls"] is None
    assert "sim_planes" not in _one(("box", "scripted", 0, 10.0, "{}", 0.85, 0.9))


def test_neither_policy_reaching_t90_is_unknown_not_a_loss():
    rooms = ("box", "furnished4")
    runs = [
        _run("box", "planner", 0, 100.0),
        _run("box", "wander", 0, 200.0),
        _run("furnished4", "planner", 0, None),  # both censored at the horizon: equal medians
        _run("furnished4", "wander", 0, -1),
    ]
    summary = summarize(runs, rooms, ("planner", "wander"), 300.0)
    assert summary["furnished4"]["planner"]["median_t90_censored"] is True
    assert summary["box"]["planner"]["median_t90_censored"] is False
    acc = acceptance(runs, summary, rooms)
    assert acc["planner_beats_wander_all_rooms"] is None
    assert acc["neither_reached_t90_rooms"] == ["furnished4"]
    assert not any("wander" in f for f in acceptance_failures(acc))
    runs[1]["t90_s"] = 50.0  # a real loss in another room still fails
    acc = acceptance(runs, summarize(runs, rooms, ("planner", "wander"), 300.0), rooms)
    assert acc["planner_beats_wander_all_rooms"] is False
