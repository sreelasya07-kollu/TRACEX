"""Instruction demand/min and the acceptance block of `ckit guide-eval` (no native core needed)."""

import json

from ckit.guide_eval import (
    SPEECH_BUDGET_PER_MIN,
    _median_or_none,
    acceptance,
    instruction_demand_per_min,
    summarize,
    table,
)


def test_instruction_demand_per_min():
    assert instruction_demand_per_min(12, 90.0) == 8.0
    assert instruction_demand_per_min(1, 7.0) == 8.57
    assert instruction_demand_per_min(0, 60.0) == 0.0
    assert instruction_demand_per_min(None, 60.0) is None
    assert instruction_demand_per_min(5, 0.0) is None
    assert instruction_demand_per_min(5, None) is None


def test_median_or_none_skips_missing():
    assert _median_or_none([None, 2.0, 4.0]) == 3.0
    assert _median_or_none([None]) is None


def _run(policy, seed, demand, t90=100.0, rep=0.1):
    return {
        "room": "box", "policy": policy, "seed": seed, "coverage_gt_final": 0.9,
        "coverage_claimed_final": 0.88, "t80_s": 50.0, "t90_s": t90, "flap_per_min": 0.5,
        "unsafe_targets_gt": 0, "repetition_rate": rep, "instruction_demand_per_min": demand,
    }  # fmt: skip


def test_acceptance_with_planner_runs():
    runs = [_run("planner", 0, 4.0), _run("planner", 1, 6.0), _run("wander", 0, None, t90=-1)]
    summary = summarize(runs, ("box",), ("planner", "wander"), 450.0)
    acc = acceptance(runs, summary, ("box",))
    assert acc["median_instruction_demand_per_min"] == 5.0
    assert acc["max_instruction_demand_per_min"] == 6.0
    assert acc["instruction_demand_within_budget"] is True
    assert acc["prompts_per_min_budget"] == SPEECH_BUDGET_PER_MIN
    assert acc["planner_beats_wander_median_t90_rooms"] == 1
    assert summary["box"]["wander"]["median_instruction_demand_per_min"] is None

    over = [_run("planner", 0, SPEECH_BUDGET_PER_MIN + 0.5)]
    acc = acceptance(over, summarize(over, ("box",), ("planner",), 60.0), ("box",))
    assert acc["instruction_demand_within_budget"] is False


def test_acceptance_without_planner_or_data_is_none():
    runs = [_run("wander", 0, 3.0)]
    acc = acceptance(runs, summarize(runs, ("box",), ("wander",), 60.0), ("box",))
    assert acc["instruction_demand_within_budget"] is None
    assert acc["median_instruction_demand_per_min"] is None
    assert acc["max_instruction_demand_per_min"] is None

    runs = [_run("planner", 0, None)]
    acc = acceptance(runs, summarize(runs, ("box",), ("planner",), 60.0), ("box",))
    assert acc["instruction_demand_within_budget"] is None


def test_table_prints_demand_and_repetition():
    runs = [_run("planner", 0, 4.5, rep=0.05), _run("wander", 0, None, rep=None)]
    summary = summarize(runs, ("box",), ("planner", "wander"), 60.0)
    lines = table({"summary": summary, "acceptance": acceptance(runs, summary, ("box",))})
    lines = lines.splitlines()
    assert "demand/min" in lines[0] and "repetition" in lines[0]
    assert lines[1].split()[-2:] == ["4.5", "0.05"]
    assert lines[2].split()[-2:] == ["-", "-"]
    assert json.loads(lines[-1])["instruction_demand_within_budget"] is True
