"""Phase 12: `ckit trial-report` over three fake e2e reports."""

import json

from typer.testing import CliRunner

from ckit.cli import app
from ckit.trial_report import aggregate, to_markdown


def _rep(name, participant, arm, dev, verdict, dur, instr, reg, reproj):
    return {
        "bundle": name, "participant": participant, "arm": arm, "room": "R1", "device": dev,
        "basics": {"duration_s": dur},
        "verdict": {"verdict": verdict},
        "guidance": {"instructions": instr, "instructions_per_min": None,
                     "repetition_rate": 0.1, "coverage_completeness": 0.8},
        "sfm": {"registered_ratio": reg, "reproj_mean_px": reproj} if reg is not None
        else {"error": "pycolmap failed"},
    }  # fmt: skip


REPORTS = [
    _rep("s1", "P01", "guided", "CPH2381", "PASS", 120.0, 10, 0.9, 0.5),
    _rep("s2", "P02", "unguided", "CPH2381", "FAIL", 200.0, 0, 0.5, 1.5),
    _rep("s3", "P01", "unguided", "Pixel6", "WARN", 100.0, 4, None, None),
]


def test_aggregate_medians_by_group():
    rep = aggregate(REPORTS)
    o = rep["overall"]
    assert o["n"] == 3 and o["usable_rate"] == round(2 / 3, 4)
    assert o["median_duration_s"] == 120.0 and o["median_instructions"] == 4
    assert o["median_registered_ratio"] == 0.7  # failed sfm section ignored
    assert o["median_instructions_per_min"] is None
    arm = rep["groups"]["arm"]
    assert arm["guided"]["n"] == 1 and arm["unguided"]["n"] == 2
    assert arm["unguided"]["median_duration_s"] == 150.0
    assert rep["groups"]["participant"]["P01"]["usable_rate"] == 1.0
    assert set(rep["groups"]["device"]) == {"CPH2381", "Pixel6"}
    assert rep["groups"]["room"]["R1"]["n"] == 3


def test_missing_group_key_is_unknown():
    r = dict(REPORTS[0])
    del r["participant"]
    assert "unknown" in aggregate([r])["groups"]["participant"]


def test_non_scalar_group_values_are_stringified():
    r = dict(REPORTS[0], device={"model": "X"}, room=["a"])
    g = aggregate([r, REPORTS[1]])["groups"]
    assert "{'model': 'X'}" in g["device"] and "['a']" in g["room"]


def test_markdown_tables():
    md = to_markdown(aggregate(REPORTS))
    assert "## arm" in md and "| guided | 1 | 1.0 |" in md


def test_cli_reads_dir_and_writes_outputs(tmp_path):
    for i, r in enumerate(REPORTS):
        d = tmp_path / "in" / f"d{i}"
        d.mkdir(parents=True)
        (d / f"{r['bundle']}.e2e.json").write_text(json.dumps(r))
    res = CliRunner().invoke(
        app,
        ["trial-report", str(tmp_path / "in"), "--json", str(tmp_path / "t.json"),
         "--html", str(tmp_path / "t.html")],
    )  # fmt: skip
    assert res.exit_code == 0, res.output
    assert "## participant" in res.output
    assert json.loads((tmp_path / "t.json").read_text())["overall"]["n"] == 3
    assert "<table>" in (tmp_path / "t.html").read_text()
    (tmp_path / "in" / "bad.e2e.json").write_text("{not json")
    (tmp_path / "in" / "list.e2e.json").write_text("[1]")
    res2 = CliRunner().invoke(app, ["trial-report", str(tmp_path / "in")])
    assert res2.exit_code == 0 and res2.output.count("skipped ") == 2, res2.output
    assert "| all | 3 |" in res2.output
    empty = CliRunner().invoke(app, ["trial-report", str(tmp_path / "none")])
    assert empty.exit_code == 1
