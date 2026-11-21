"""Phase 4: quality sink through replay, keyframe-selected import, quality-report."""

import json
from pathlib import Path

import pytest
from typer.testing import CliRunner

from ckit.bundle.import_raw import import_raw, import_raw_selected
from ckit.bundle.streams import read_streams
from ckit.bundle.validate import validate
from ckit.cli import app
from ckit.quality_report import quality_report
from ckit.replay.driver import Faults, dumps, replay

RAW = Path(__file__).parent / "fixtures" / "raw_small"


@pytest.fixture(scope="module")
def bundle(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("q") / "bundle"
    import_raw(RAW, out)
    return out


def test_quality_sink_report_deterministic(bundle):
    a = replay(bundle, ["metrics", "quality"], Faults(seed=1, jitter_ms=1))
    b = replay(bundle, ["metrics", "quality"], Faults(seed=1, jitter_ms=1))
    assert dumps(a) == dumps(b)
    q = a["sinks"]["quality"]
    assert q["frames"] == a["input"]["delivered"]["frame"]
    assert 0 < q["keyframes"] <= q["frames"]
    assert len(q["selected_frame_ids"]) == q["keyframes"]
    assert q["config_version"] == "q1" and len(q["config_hash"]) == 16


def test_config_override_changes_hash_and_selection(bundle):
    base = replay(bundle, ["quality"])["sinks"]["quality"]
    strict = replay(bundle, ["quality"], sink_config={"quality": {"rot_deg": 45, "trans_ratio": 5}})
    s = strict["sinks"]["quality"]
    assert s["config_hash"] != base["config_hash"]
    assert s["keyframes"] <= base["keyframes"]
    with pytest.raises(ValueError):
        replay(bundle, ["quality"], sink_config={"quality": {"bogus": 1}})


def test_tracking_loss_frames_rejected(bundle):
    rep = replay(
        bundle, ["quality"], Faults(tracking_loss=[(0.0, 100.0)]), sink_config={"quality": {}}
    )
    q = rep["sinks"]["quality"]
    assert q["keyframes"] == 0
    assert q["hard_reject_histogram"].get("TRACKING_LOST", 0) == q["frames"]


def test_import_with_selection_writes_metrics_and_validates(tmp_path):
    out = tmp_path / "sel"
    counts = import_raw_selected(RAW, out)
    s = read_streams(out / "streams.mcap")
    frames = s.get("/frame_index")
    kept = [f for f in frames if f.keyframe]
    assert len(kept) == counts["selected_keyframes"]
    assert len(list((out / "keyframes").glob("*.jpg"))) == len(kept)
    assert all(f.file for f in kept) and not any(f.file for f in frames if not f.keyframe)
    assert any(f.sharpness > 0 for f in frames)
    assert any(f.reasons for f in frames if not f.keyframe)
    codes = {e.code for e in s.get("/events")}
    assert "QUALITY_CONFIG" in codes
    m = json.loads((out / "manifest.json").read_text())
    settings = m["capture_settings"] if "capture_settings" in m else m
    assert "quality_config_hash" in json.dumps(settings)
    rep = validate(out, "quality")
    assert rep.verdict != "FAIL", rep.summary()


def test_quality_report_cli_no_sfm(bundle, tmp_path):
    r = CliRunner().invoke(
        app, ["quality-report", str(bundle), "--no-sfm", "--json", str(tmp_path / "q.json")]
    )
    assert r.exit_code == 0, r.output
    d = json.loads((tmp_path / "q.json").read_text())["bundles"][0]
    assert "reason_histogram" in d and "sfm" not in d


def test_quality_report_registration_small(bundle):
    rep = quality_report(bundle)
    if "sfm" in rep:
        assert 0.0 <= rep["sfm"]["registration_rate"] <= 1.0
