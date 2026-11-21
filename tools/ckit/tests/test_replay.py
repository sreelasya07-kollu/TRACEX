"""Phase 3: replay determinism, ordering, fault injection, clocks, compare."""

import json
from pathlib import Path

import pytest
from typer.testing import CliRunner

from ckit.bundle.import_raw import import_raw
from ckit.cli import app
from ckit.replay.compare import compare
from ckit.replay.driver import RANK, FakeClock, Faults, dumps, inject, replay


@pytest.fixture(scope="module")
def bundle(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("rp") / "bundle"
    import_raw(Path(__file__).parent / "fixtures" / "raw_small", out)
    return out


def test_two_runs_byte_identical_via_cli(bundle, tmp_path):
    args = ["replay", str(bundle), "--seed", "3", "--jitter-ms", "2", "--drop-frames", "0.2"]
    r = CliRunner()
    a = r.invoke(app, [*args, "--out", str(tmp_path / "a.json")])
    b = r.invoke(app, [*args, "--out", str(tmp_path / "b.json")])
    assert a.exit_code == 0 and b.exit_code == 0, a.output + b.output
    assert (tmp_path / "a.json").read_bytes() == (tmp_path / "b.json").read_bytes()
    c = r.invoke(app, ["replay-compare", str(tmp_path / "a.json"), str(tmp_path / "b.json")])
    assert c.exit_code == 0


def test_seed_changes_faults(bundle):
    a = replay(bundle, faults=Faults(drop_frames=0.5, seed=1))
    b = replay(bundle, faults=Faults(drop_frames=0.5, seed=2))
    assert dumps(a) != dumps(b)


def test_tracking_loss_injection_shows_interval_and_event(bundle):
    rep = replay(bundle, faults=Faults(tracking_loss=[(0.5, 1.0)]))
    m = rep["sinks"]["metrics"]
    assert m["events"]["replay.inject.tracking_loss"] == 1
    lost = [iv for iv in m["tracking_not_normal"] if iv["state"] == 3]
    assert lost and lost[0]["t0_s"] == pytest.approx(0.5, abs=0.05)
    assert lost[0]["t1_s"] == pytest.approx(1.0, abs=0.05)
    assert rep["input"]["injected"]["poses_lost"] > 0


def test_thermal_injection_is_a_platform_event(bundle):
    rep = replay(bundle, faults=Faults(thermal=[(0.2, "serious"), (0.4, "nominal")]))
    assert rep["sinks"]["metrics"]["events"]["platform.thermal"] == 2


def test_drop_and_jitter(bundle):
    base = replay(bundle)
    rep = replay(bundle, faults=Faults(drop_frames=1.0))
    assert rep["sinks"]["metrics"]["channels"].get("frame", {}).get("n", 0) == 0
    assert rep["input"]["injected"]["frames_dropped"] == base["input"]["delivered"]["frame"]
    j = replay(bundle, faults=Faults(jitter_ms=20, seed=5))
    ch = j["sinks"]["metrics"]["channels"]
    assert ch["gyro"]["rejected_non_monotonic"] > 0  # out-of-order delivery is caught by the core


def test_tie_break_order():
    ev = [
        (5, RANK["planes"], 0, "planes", {}),
        (5, RANK["pose"], 0, "pose", {"t_ns": 5}),
        (5, RANK["intrinsics"], 0, "intrinsics", {}),
        (4, RANK["event"], 0, "event", (4, "a", "b", "", [])),
    ]
    out, _ = inject(ev, Faults())
    assert [e[3] for e in out] == ["event", "intrinsics", "pose", "planes"]


def test_realtime_uses_injected_clock(bundle):
    clk = FakeClock()
    fast = replay(bundle)
    rt = replay(bundle, realtime=True, speed=2.0, clock=clk)
    assert clk.sleeps > 0
    dur = fast["sinks"]["metrics"]["duration_s"]
    assert clk.t / 1e9 == pytest.approx(dur / 2.0, rel=0.05)
    rt["options"]["mode"] = "fast"
    assert dumps(rt) == dumps(fast)  # mode only changes pacing, never results


def test_compare_tolerances():
    a = {"x": 1.0, "s": "a", "l": [1, 2], "path_length_m": 10.0}
    assert compare(a, dict(a, x=1.0 + 1e-9)) == []
    assert compare(a, dict(a, x=1.1))
    assert compare(a, dict(a, l=[1]))
    assert compare(a, dict(a, path_length_m=10.005), key_tol={"path_length_m": 0.01}) == []


def test_unknown_sink_raises(bundle):
    with pytest.raises(ValueError):
        replay(bundle, sinks=["nope"])


def test_plot(bundle, tmp_path):
    r = CliRunner().invoke(app, ["replay-plot", str(bundle), str(tmp_path / "t.png")])
    assert r.exit_code == 0 and (tmp_path / "t.png").stat().st_size > 1000


def test_report_has_no_wall_clock(bundle):
    text = json.dumps(replay(bundle))
    assert "wall" not in text and "elapsed" not in text
