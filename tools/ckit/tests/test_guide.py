"""Phase 8: guidance on recorded bundles (replay sink), in the live session (events + guidance_log),
and guide-eval."""

import json
from pathlib import Path

import pytest
from typer.testing import CliRunner

from ckit.bundle.import_raw import import_raw
from ckit.bundle.streams import read_streams
from ckit.bundle.validate import validate
from ckit.cli import app
from ckit.core import load
from ckit.replay.driver import Faults, dumps, replay
from ckit.simulate import simulate

RAW = Path(__file__).parent / "fixtures" / "raw_small"


@pytest.fixture(scope="module")
def bundle(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("guide") / "bundle"
    import_raw(RAW, out)
    return out


def test_guide_sink_report_structure_and_determinism(bundle):
    a = replay(bundle, ["guide"], Faults(seed=2, jitter_ms=1))
    b = replay(bundle, ["guide"], Faults(seed=2, jitter_ms=1))
    assert dumps(a) == dumps(b)
    g = a["sinks"]["guide"]["engine"]
    assert g["tier"] in ("B", "C") and len(g["params_hash"]) == 16
    assert 0.0 <= g["coverage"]["completeness"] <= 1.0
    assert g["log"]["unsafe_targets"] == 0
    assert g["log"]["repetition_rate"] <= 0.5


def test_guide_params_override_and_validation(bundle):
    base = replay(bundle, ["guide"])["sinks"]["guide"]["engine"]["params_hash"]
    other = replay(bundle, ["guide"], sink_config={"guide": {"min_views": 5}})
    assert other["sinks"]["guide"]["engine"]["params_hash"] != base
    with pytest.raises(ValueError):
        replay(bundle, ["guide"], sink_config={"guide": {"no_such_param": 1}})
    g = load().guide
    assert g.params_hash("{}") == base
    with pytest.raises(ValueError):
        g.params_json('{"bogus": 1}')


def test_session_writes_guidance_log_and_events(bundle, tmp_path):
    out = tmp_path / "scan"
    simulate(bundle, out)
    assert (out / "guidance_log.json").exists()
    log = json.loads((out / "guidance_log.json").read_text())
    assert log["schema"] == "capturekit.guidance_log/1" and len(log["params_hash"]) == 16
    m = json.loads((out / "manifest.json").read_text())
    assert "guidance_log.json" in {f["path"] for f in m["files"]}  # hashed with the rest
    assert "guide_config_hash" in json.dumps(m)
    rep = validate(out, "quality")
    assert rep.verdict != "FAIL", rep.summary()
    codes = {e.code for e in read_streams(out / "streams.mcap").get("/events")}
    if log["instructions"]:
        assert "GUIDE_INSTRUCTION" in codes


def test_session_guide_config_keys(bundle, tmp_path):
    out = tmp_path / "scan2"
    simulate(bundle, out, config={"guide.min_views": 5})
    log = json.loads((out / "guidance_log.json").read_text())
    assert log["params_hash"] == load().guide.params_hash('{"min_views": 5}')
    with pytest.raises(RuntimeError):
        simulate(bundle, tmp_path / "bad", config={"guide.nope": 1})


def test_guide_eval_cli_bundle_and_simulation(bundle, tmp_path):
    r = CliRunner().invoke(app, ["guide-eval", str(bundle), "--json", str(tmp_path / "b.json")])
    assert r.exit_code == 0, r.output
    rows = json.loads((tmp_path / "b.json").read_text())["bundles"]
    assert rows[0]["unsafe_targets"] == 0
    r = CliRunner().invoke(
        app,
        ["guide-eval", "--rooms", "box", "--policies", "scripted,wander", "--seeds", "1",
         "--max-s", "20", "--workers", "1", "--json", str(tmp_path / "s.json")],
    )  # fmt: skip
    assert r.exit_code == 0, r.output
    rep = json.loads((tmp_path / "s.json").read_text())
    assert len(rep["runs"]) == 2 and rep["acceptance"]["unsafe_targets_gt_total"] == 0


def test_simulator_is_deterministic_per_seed():
    g = load().guide
    a = g.simulate("box", "wander", 5, 20.0)
    assert a == g.simulate("box", "wander", 5, 20.0)
    assert a != g.simulate("box", "wander", 6, 20.0)
    with pytest.raises(Exception):  # noqa: B017 - unknown params are rejected
        g.simulate("box", "wander", 0, 5.0, '{"bogus": 1}')
