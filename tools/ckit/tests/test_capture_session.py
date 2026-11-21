"""Phase 6: the real C API driven like a phone (ctypes): valid bundles, determinism, governor pause,
threaded mode, and forced-kill (SIGKILL) recovery."""

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import pytest

from ckit import capi
from ckit.bundle.import_raw import import_raw
from ckit.bundle.streams import read_streams
from ckit.bundle.validate import validate
from ckit.simulate import simulate

RAW = Path(__file__).parent / "fixtures" / "raw_small"


@pytest.fixture(scope="module")
def source(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("sim_src") / "src"
    import_raw(RAW, out)
    return out


def _sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def test_simulated_capture_validates_clean(source, tmp_path):
    out = tmp_path / "b"
    r = simulate(source, out)
    assert r["state"] == "COMPLETE" and r["finalize_status"] == 0
    assert r["keyframe_requests"] > 0 and r["acked"] == r["keyframe_requests"]
    rep = validate(out, "quality")
    assert rep.verdict != "FAIL", rep.summary()
    assert not rep.codes("ERROR")  # the tiny fixture legitimately warns FEW_KEYFRAMES
    s = read_streams(out / "streams.mcap")
    kf = [f for f in s.get("/frame_index") if f.keyframe]
    assert len(kf) == r["acked"] and all((out / f.file).exists() for f in kf)
    assert {"SESSION_STARTED", "QUALITY_CONFIG", "SESSION_FINALIZED"} <= {
        e.code for e in s.get("/events")
    }
    m = json.loads((out / "manifest.json").read_text())
    assert "quality_config_hash" in json.dumps(m)


def test_core_session_is_deterministic(source, tmp_path):
    a, b = tmp_path / "a" / "bundle", tmp_path / "b" / "bundle"
    simulate(source, a)
    simulate(source, b)
    assert _sha(a / "streams.mcap") == _sha(b / "streams.mcap")
    assert _sha(a / "manifest.json") == _sha(b / "manifest.json")


def test_threaded_mode_produces_valid_bundle(source, tmp_path):
    out = tmp_path / "t"
    r = simulate(source, out, threaded=True)
    assert r["state"] == "COMPLETE"
    assert validate(out, "quality").verdict != "FAIL"


def test_critical_thermal_pauses_session_and_bundle_stays_valid(source, tmp_path):
    out = tmp_path / "hot"
    r = simulate(source, out, platform_events=[(0.5, {"thermal": 3})])
    assert r["final_policy_level"] == 5
    s = read_streams(out / "streams.mcap")
    codes = [e.code for e in s.get("/events")]
    assert "GOVERNOR_LEVEL" in codes and codes.count("SESSION_PAUSED") >= 1
    # paused before the first keyframe: structurally valid, but 0 keyframes is ERROR NO_KEYFRAMES
    assert validate(out, "quality").codes("ERROR") <= {"NO_KEYFRAMES"}


def test_invalid_config_and_double_create(source, tmp_path):
    with pytest.raises(RuntimeError):
        simulate(source, tmp_path / "x", config={"nope": 1})
    out = tmp_path / "y"
    simulate(source, out)
    import ctypes as C

    err = C.c_int(0)
    assert not capi.lib().ck_session_create(str(out).encode(), b"{}", C.byref(err))
    assert err.value == capi.ERR_IO


@pytest.mark.parametrize("kill_after", [60, 300, 700, 1000])
def test_forced_kill_then_recover_gives_valid_partial_bundle(source, tmp_path, kill_after):
    out = tmp_path / f"k{kill_after}"
    code = (
        "from pathlib import Path\n"
        "from ckit.simulate import simulate\n"
        f"simulate(Path({str(source)!r}), Path({str(out)!r}), kill_after_events={kill_after})\n"
    )
    p = subprocess.run([sys.executable, "-c", code], capture_output=True)
    assert p.returncode == -9, p.stderr.decode()[-500:]  # killed by SIGKILL, nothing finalized
    assert not (out / "manifest.json").exists()
    st, rep = capi.recover(out)
    if st != 0:  # killed before the first chunk was flushed (about 1 s of event time): all lost
        assert "unreadable" in rep["error"] or "nothing recoverable" in rep["error"]
        assert kill_after <= 400
        return
    assert rep["ok"] and rep["poses"] > 0
    assert rep["poses"] >= 0.3 * kill_after / 12  # at most about 1 s of data lost
    v = validate(out, "quality")
    # killed before any keyframe landed: structurally valid, 0 keyframes is the only ERROR allowed
    assert v.codes("ERROR") <= {"NO_KEYFRAMES"}, v.summary()
    assert "PARTIAL_BUNDLE" in v.codes()
    assert not (out / "session.json").exists()
    kfs = {f.file for f in read_streams(out / "streams.mcap").get("/frame_index") if f.keyframe}
    on_disk = {f"keyframes/{p.name}" for p in (out / "keyframes").glob("*.jpg")}
    assert kfs == on_disk  # no dangling references, no orphan files
    st2, rep2 = capi.recover(out)
    assert st2 == 0 and rep2["already_complete"]


def test_ingest_detections_via_capi_reaches_bundle(tmp_path):
    import ctypes as C

    L = capi.lib()
    out = tmp_path / "det"
    err = C.c_int(0)
    s = L.ck_session_create(str(out).encode(), b'{"threaded":0}', C.byref(err))
    assert s
    boxes = (capi.Detection * 2)(
        capi.Detection(3, 0.75, 10, 20, 110, 220), capi.Detection(1, 0.5, 0, 0, 50, 60)
    )
    assert L.ck_ingest_detections(s, 1, 0, boxes, 2) == capi.ERR_STATE
    assert L.ck_session_start(s, 0) == capi.OK
    assert L.ck_session_pump(s) == capi.OK
    assert L.ck_ingest_detections(s, 100, 7, boxes, 2) == capi.OK
    assert L.ck_ingest_detections(s, 200, 8, None, 0) == capi.OK
    assert L.ck_ingest_detections(s, 300, 9, None, 2) == capi.ERR_ARG
    assert L.ck_session_pump(s) == capi.OK
    L.ck_session_finalize(s)
    L.ck_session_destroy(s)
    dets = read_streams(out / "streams.mcap").get("/ar/detections")
    assert len(dets) == 2
    assert dets[0].frame_id == 7 and len(dets[0].boxes) == 2 and dets[0].boxes[0].cls == 3
    assert len(dets[1].boxes) == 0


def test_understanding_on_bundle_validates_with_placeholder_consent(source, tmp_path):
    """ADR 0028: 1.3.0 bundles need consent; simulate() stamps placeholders for synthetic runs."""
    out = tmp_path / "und"
    # record_imu keeps the 1.3.0 shape; the lean default (1.4.0) is in test_lean_bundle.py
    r = simulate(source, out, {"guide.understanding": 1, "record_imu": 1})
    assert r["state"] == "COMPLETE"
    m = json.loads((out / "manifest.json").read_text())
    assert m["schema_version"] == "1.3.0" and m["counts"]["guide_shell"] > 0, m["counts"]
    assert m["consent"]["version"] == "synthetic" and m["privacy"]["redaction_mode"] == "none"
    rep = validate(out, "quality")
    assert rep.codes("ERROR") == set(), rep.summary()
