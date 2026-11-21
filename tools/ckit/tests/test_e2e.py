"""Phase 12: `ckit e2e` per-scan report (sections fail independently) and golden-bundle run."""

import json
from pathlib import Path

import pytest
from typer.testing import CliRunner

from ckit.cli import app
from ckit.e2e import e2e, to_html, write

RAW = Path(__file__).parent / "fixtures" / "raw_small"


def _fake_bundle(root: Path) -> Path:
    b = root / "scan_a"
    b.mkdir()
    manifest = {
        "schema_version": "1.0.0",
        "bundle_id": "b1",
        "device": {"platform": "android", "model": "CPH2381", "os": "14", "app_version": "0.1"},
        "clock": {"base": "mono", "offset_ns": 0, "t_start_ns": 1_000_000_000,
                  "t_end_ns": 91_000_000_000},
        "counts": {"frames": 400, "keyframes": 40},
        "completion": {"status": "complete"},
        "files": [],
    }  # fmt: skip
    (b / "manifest.json").write_text(json.dumps(manifest))
    research = {"participant": "P01", "arm": "guided", "room": "R1",
                "intents": ["pause", "resume", "pause"]}  # fmt: skip
    (root / "scan_a.research.json").write_text(json.dumps(research))
    return b


def test_e2e_sections_and_research_beside_bundle(tmp_path):
    rep = e2e(_fake_bundle(tmp_path), sfm=False, guide=False)
    assert rep["basics"]["duration_s"] == 90.0 and rep["basics"]["keyframes"] == 40
    assert rep["device"] == "CPH2381"
    assert rep["verdict"]["verdict"] == "FAIL"  # no streams/camera: validator says rescan
    assert (rep["participant"], rep["arm"], rep["room"]) == ("P01", "guided", "R1")
    assert rep["research"]["intents"] == {"pause": 2, "resume": 1}
    assert rep["sfm"] == {"skipped": True} and rep["guidance"] == {"skipped": True}
    assert rep["missing_area_recall"] is None


def test_e2e_failing_section_is_recorded_not_raised(tmp_path):
    b = _fake_bundle(tmp_path)
    rep = e2e(b, research=tmp_path / "missing.json", sfm=True, guide=True)
    assert "error" in rep["research"]
    assert "error" in rep["sfm"]  # no keyframes / streams
    assert "error" in rep["guidance"]  # no streams.mcap to replay
    assert rep["participant"] is None


def test_e2e_html_escapes_and_cli_writes_files(tmp_path):
    b = _fake_bundle(tmp_path)
    rep = e2e(b, sfm=False, guide=False)
    rep["basics"]["model_note"] = "<b>&"
    page = to_html(rep)
    assert "&lt;b&gt;&amp;" in page and "<b>&" not in page
    out = tmp_path / "out"
    r = CliRunner().invoke(app, ["e2e", str(b), "--no-sfm", "--no-guide", "--out", str(out)])
    assert r.exit_code == 0, r.output
    assert "verdict=FAIL" in r.output and "duration_s=90.0" in r.output
    d = json.loads((out / "scan_a.e2e.json").read_text())
    assert d["e2e_version"] == "e2e1"
    assert (out / "scan_a.e2e.html").exists()


def test_cli_research_option_with_intent_dicts(tmp_path):
    b = _fake_bundle(tmp_path)
    log = tmp_path / "log.json"
    intents = [{"intent": "pause", "ms": 400}, {"intent": "finish"}, {"intent": "pause"}]
    log.write_text(json.dumps({"participant": "P09", "arm": "unguided", "intents": intents}))
    r = CliRunner().invoke(
        app,
        ["e2e", str(b), "--research", str(log), "--no-sfm", "--no-guide", "--out", str(tmp_path)],
    )
    assert r.exit_code == 0, r.output
    d = json.loads((tmp_path / "scan_a.e2e.json").read_text())
    assert d["research"]["intents"] == {"pause": 2, "finish": 1}
    assert (d["participant"], d["arm"]) == ("P09", "unguided")


@pytest.fixture(scope="module")
def golden(tmp_path_factory) -> Path:
    try:
        from ckit.core import load

        load()
    except ImportError as e:
        pytest.skip(f"_ckcore not built: {e}")
    from ckit.bundle.import_raw import import_raw

    out = tmp_path_factory.mktemp("e2e") / "bundle"
    import_raw(RAW, out)
    return out


def test_e2e_on_golden_bundle(golden, tmp_path):
    rep = e2e(golden)  # one small bundle: the only pycolmap run in this file
    assert rep["verdict"]["verdict"] in ("PASS", "WARN")
    assert rep["basics"]["duration_s"] > 0
    assert isinstance(rep["guidance"]["instructions"], int), rep["guidance"]
    assert "error" not in rep["sfm"], rep["sfm"]
    assert 0.0 <= rep["sfm"]["registered_ratio"] <= 1.0
    j, h = write(rep, tmp_path)
    assert json.loads(j.read_text())["bundle"] == "bundle" and h.exists()


def test_score_gaps_recall_and_false_rate():
    from ckit.e2e import score_gaps

    truth = [{"x0": 0, "z0": 0, "x1": 1, "z1": 0}, {"x0": 10, "z0": 10, "x1": 11, "z1": 10}]
    gaps = [{"x0": 0.4, "z0": 0.3, "x1": 1.2, "z1": 0.3}, {"x0": 5, "z0": 5, "x1": 6, "z1": 5}]
    s = score_gaps(gaps, truth)
    assert (s["n_truth"], s["recalled"], s["recall"]) == (2, 1, 0.5)
    assert (s["n_gaps"], s["false_gaps"], s["false_gap_rate"]) == (2, 1, 0.5)
    assert score_gaps([], truth)["recall"] == 0.0 and score_gaps([], [])["recall"] is None


def test_e2e_understanding_from_guide_channels_and_truth(tmp_path):
    try:
        from ckit.core import load

        ck = load()
    except ImportError as e:
        pytest.skip(f"_ckcore not built: {e}")
    b = _fake_bundle(tmp_path)
    w = ck.bundle.StreamWriter()
    assert w.open(str(b / "streams.mcap"))
    w.write_labels(10, [(1, 1, 0.0, 0.4, 0.0, 1.0, 0.8, 1.0, 0.0, 0.7, 3)], [])
    w.write_shell(
        10, [("W1", 0.0, 0.0, 4.0, 0.0, False)], [0.0, 0.0, 4.0, 0.0], False, True, 2.5, True
    )
    w.write_gaps(10, [(0, 4.0, 0.0, 4.0, 3.0, 0.8, "window wall")])
    w.close()
    (tmp_path / "scan_a.truth.json").write_text(
        json.dumps({"missing_areas": [{"x0": 4, "z0": 1, "x1": 4, "z1": 2}]})
    )
    rep = e2e(b, sfm=False, guide=False)
    u = rep["understanding"]
    assert u["present"] and u["objects"] == 1 and u["walls"] == 1 and u["gaps"] == 1
    assert u["gaps_by_type"] == {"missing_wall": 1}
    assert rep["missing_area_recall"] == 1.0 and u["gap_score"]["false_gap_rate"] == 0.0
    assert "understanding" in to_html(rep)
