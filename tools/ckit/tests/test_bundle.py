"""Phase 1: valid and broken bundles must produce the expected validator codes."""

import json
import shutil
from pathlib import Path

import numpy as np
import pytest

from ckit.bundle.import_raw import import_raw, to_internal_pose
from ckit.bundle.streams import read_streams
from ckit.bundle.validate import validate
from ckit.core import load
from ckit.raw import RawSession
from ckit.replay.driver import RANK, Faults, dumps, inject, replay
from ckit.rot import F_FLIP, quat_to_mat

ck = load()


@pytest.fixture(scope="module")
def good(
    tmp_path_factory,
) -> Path:
    raw = Path(__file__).parent / "fixtures" / "raw_small"
    out = tmp_path_factory.mktemp("good") / "bundle"
    import_raw(raw, out)
    return out


def _copy(good: Path, tmp_path: Path) -> Path:
    b = tmp_path / "b"
    shutil.copytree(good, b)
    return b


def _refinalize(b: Path) -> None:
    """Rewrite manifest hashes after a deliberate stream edit (so only the semantic issue shows)."""
    m = json.loads((b / "manifest.json").read_text())
    files = ck.bundle.list_files(str(b))
    m["files"] = files
    (b / "manifest.json").write_text(json.dumps(m, indent=2))


def _rewrite_streams(b: Path, mutate, detections=None, events=(), guide=None) -> None:
    """Re-encode streams.mcap via the C++ writer; mutate() edits poses/frames/intrinsics."""
    src = ck.bundle.read_streams(str(b / "streams.mcap"))
    poses, frames, intr = src["poses"], src["frames"], src["intrinsics"]
    poses, frames, intr = mutate(poses, frames, intr)
    w = ck.bundle.StreamWriter()
    assert w.open(str(b / "streams.mcap"))
    for k in intr:
        w.write_intrinsics(k["t_ns"], k["width"], k["height"], k["fx"], k["fy"], k["cx"], k["cy"])
    for p in poses:
        w.write_pose(p["t_ns"], p["p"], p["q"], p["state"], p["reason"])
    for t, x, y, z in src["gyro"]:
        w.write_gyro(t, x, y, z)
    for t, x, y, z in src["accel"]:
        w.write_accel(t, x, y, z)
    for f in frames:
        w.write_frame(f)
    w.write_event(1, "test", "X")
    for t, code in events:
        w.write_event(t, "session", code)
    for t, fid, boxes in detections or []:
        w.write_detections(t, fid, boxes)
    if guide:
        guide(w)
    w.close()
    _refinalize(b)


def test_valid_bundle_has_no_errors(good) -> None:
    rep = validate(good, "quality")
    assert rep.codes("ERROR") == set(), rep.summary()


def test_roundtrip_matches_raw(good) -> None:
    raw = RawSession.load(Path(__file__).parent / "fixtures" / "raw_small")
    st = read_streams(good / "streams.mcap")
    poses = st.get("/pose")
    assert len(poses) == len(raw.poses)
    for rp, bp in zip(raw.poses, poses, strict=True):
        p, q = to_internal_pose(rp["p"], rp["q"], "arkit")
        np.testing.assert_allclose([bp.tx, bp.ty, bp.tz], p, atol=1e-12)
        np.testing.assert_allclose([bp.qw, bp.qx, bp.qy, bp.qz], q, atol=1e-12)
        # The internal camera looks down +Z, the ARKit camera down -Z.
        R_int = quat_to_mat([bp.qw, bp.qx, bp.qy, bp.qz])
        np.testing.assert_allclose(R_int, quat_to_mat(rp["q"]) @ F_FLIP, atol=1e-9)
    assert len(st.get("/imu/gyro")) == sum(r["type"] == "gyro" for r in raw.imu)
    assert len(st.get("/frame_index")) == len(raw.frames)
    assert st.get("/intrinsics")[0].fx == raw.intrinsics[0]["fx"]


def _with_detections(good: Path, tmp_path: Path, make) -> Path:
    """Copy of the good bundle with /ar/detections = make(keyframes): (t_ns, frame_id, boxes)."""
    b = _copy(good, tmp_path)
    kfs = [f for f in ck.bundle.read_streams(str(b / "streams.mcap"))["frames"] if f["keyframe"]]
    dets = make(kfs)
    _rewrite_streams(b, lambda p, f, i: (p, f, i), detections=dets)
    m = json.loads((b / "manifest.json").read_text())
    m["counts"]["detections"] = len(dets)
    (b / "manifest.json").write_text(json.dumps(m, indent=2))
    return b


def test_detections_roundtrip_and_clean_validate(good, tmp_path) -> None:
    boxes = [(3, 0.875, 10.5, 20.25, 100.0, 200.0), (-1, 0.25, 0.0, 0.0, 1.0, 1.0)]
    b = _with_detections(
        good,
        tmp_path,
        lambda kfs: (
            [(k["t_ns"], k["frame_id"], boxes) for k in kfs[:2]]
            + [(kfs[2]["t_ns"], kfs[2]["frame_id"], [])]
        ),
    )
    got = read_streams(b / "streams.mcap").get("/ar/detections")  # independent protobuf reader
    assert len(got) == 3 and len(got[0].boxes) == 2 and len(got[2].boxes) == 0
    assert (got[0].boxes[0].cls, got[0].boxes[0].score, got[0].boxes[0].x0) == (3, 0.875, 10.5)
    assert (got[0].boxes[1].cls, got[0].boxes[1].x0, got[0].boxes[1].x1) == (-1, 0.0, 1.0)
    rep = validate(b, "semantic")
    assert not {
        c
        for c in rep.codes("WARN") | rep.codes("INFO")
        if c.startswith("DETECTION") or c == "COUNT_MISMATCH"
    }
    back = ck.bundle.read_streams(str(b / "streams.mcap"))["detections"]
    assert back[0]["boxes"][0] == (3, 0.875, 10.5, 20.25, 100.0, 200.0)


def test_detection_rules_and_cpp_parity(good, tmp_path) -> None:
    def make(kfs):
        t0, t1, t2 = kfs[0], kfs[1], kfs[2]
        return [
            (
                t0["t_ns"],
                t0["frame_id"],
                [(1, 0.5, 50.0, 50.0, 40.0, 60.0), (1, 1.5, 1.0, 1.0, 4.0, 6.0)],
            ),
            (
                t1["t_ns"],
                t1["frame_id"],
                [(1, float("nan"), 1.0, 1.0, 4.0, 6.0), (2, 0.9, 1.0, 1.0, 4.0, 6.0)],
            ),
            (t2["t_ns"], 99999, []),
        ]

    b = _with_detections(good, tmp_path, make)
    rep = validate(b, "semantic")
    assert "DETECTION_BAD_BOX" in rep.codes("WARN"), rep.summary()
    assert "DETECTION_UNKNOWN_FRAME" in rep.codes("INFO"), rep.summary()
    assert rep.verdict == "WARN"
    cpp = {
        (i["code"], i["severity"])
        for i in json.loads(ck.bundle.validate_semantic(str(b)))["issues"]
    }
    py = {(i["code"], i["severity"]) for i in rep.to_json()["issues"] if i["level"] == "semantic"}
    assert cpp == py, cpp ^ py


def _cpp_py_semantic(b: Path):
    rep = validate(b, "semantic")
    cpp = {
        (i["code"], i["severity"])
        for i in json.loads(ck.bundle.validate_semantic(str(b)))["issues"]
    }
    py = {(i["code"], i["severity"]) for i in rep.to_json()["issues"] if i["level"] == "semantic"}
    # COUNT_MISMATCH needs the manifest counts, which the C++ semantic pass does not see here.
    return rep, cpp, py - {("COUNT_MISMATCH", "WARN")}


def test_time_zero_pose_or_intrinsics_is_error(good, tmp_path) -> None:
    def zero_pose(poses, frames, intr):
        poses[0]["t_ns"] = 0
        return poses, frames, intr

    def zero_intr(poses, frames, intr):
        intr[0]["t_ns"] = 0
        return poses, frames, intr

    for i, mutate in enumerate((zero_pose, zero_intr)):
        b = tmp_path / f"tz{i}"
        shutil.copytree(good, b)
        _rewrite_streams(b, mutate)
        rep, cpp, py = _cpp_py_semantic(b)
        assert "TIME_ZERO" in rep.codes("ERROR"), rep.summary()
        assert rep.verdict == "FAIL"
        assert ("TIME_ZERO", "ERROR") in cpp and cpp == py, cpp ^ py


def test_zero_keyframes_is_error_few_is_warn(good, tmp_path) -> None:
    def keep(n):
        def m(poses, frames, intr):
            seen = 0
            for f in frames:
                if f["keyframe"]:
                    seen += 1
                    if seen > n:
                        f["keyframe"], f["file"] = False, ""
            return poses, frames, intr

        return m

    for n, code, sev, absent in (
        (0, "NO_KEYFRAMES", "ERROR", "FEW_KEYFRAMES"),
        (2, "FEW_KEYFRAMES", "WARN", "NO_KEYFRAMES"),
    ):
        b = tmp_path / f"k{n}"
        shutil.copytree(good, b)
        _rewrite_streams(b, keep(n))
        rep, cpp, py = _cpp_py_semantic(b)
        assert code in rep.codes(sev), rep.summary()
        assert absent not in {i["code"] for i in rep.to_json()["issues"]}, rep.summary()
        assert (code, sev) in cpp and cpp == py, cpp ^ py
    assert validate(good, "semantic").verdict != "FAIL"


def test_pose_gap_ignores_recorded_pauses(good, tmp_path) -> None:
    poses = ck.bundle.read_streams(str(good / "streams.mcap"))["poses"]
    t_a = poses[len(poses) // 3]["t_ns"]
    t_b = t_a + 1_200_000_000
    assert poses[-1]["t_ns"] > t_b + 100_000_000, "fixture too short for this test"

    def cut(poses, frames, intr):  # a 1.2 s hole in /pose (keyframes inside it are not under test)
        return [p for p in poses if not (t_a < p["t_ns"] < t_b)], frames, intr

    def gap_codes(name, events):
        b = tmp_path / name
        shutil.copytree(good, b)
        _rewrite_streams(b, cut, events=events)
        rep, cpp, py = _cpp_py_semantic(b)
        assert cpp == py, cpp ^ py
        return {i["code"] for i in rep.to_json()["issues"]}

    assert "POSE_GAP" in gap_codes("nopause", [])
    first_after = min(p["t_ns"] for p in poses if p["t_ns"] >= t_b)
    assert "POSE_GAP" not in gap_codes(
        "paused", [(t_a + 1, "SESSION_PAUSED"), (first_after, "SESSION_RESUMED")]
    )
    # Pause still open at the end of the stream covers the rest of the gap too.
    assert "POSE_GAP" not in gap_codes("open", [(t_a + 1, "SESSION_PAUSED")])
    # A pause covering only a small part of the hole leaves a real gap > 500 ms.
    assert "POSE_GAP" in gap_codes(
        "short", [(t_a + 1, "SESSION_PAUSED"), (t_a + 200_000_000, "SESSION_RESUMED")]
    )


def test_replay_delivers_detections_twice_byte_identical(good, tmp_path) -> None:
    boxes = [(3, 0.875, 10.5, 20.25, 100.0, 200.0)]
    b = _with_detections(
        good, tmp_path, lambda kfs: [(k["t_ns"], k["frame_id"], boxes) for k in kfs[:3]]
    )
    a, c = dumps(replay(b)), dumps(replay(b))
    assert a == c
    assert json.loads(a)["sinks"]["metrics"]["channels"]["detections"]["n"] == 3
    # a bundle without the channel reports no detections channel
    assert "detections" not in json.loads(dumps(replay(good)))["sinks"]["metrics"]["channels"]
    ev = [(5, RANK["detections"], 0, "detections", {}), (5, RANK["planes"], 0, "planes", {})]
    assert [e[3] for e in inject(ev, Faults())[0]] == ["planes", "detections"]


def test_frozen_1_0_0_still_validates() -> None:
    from ckit.bundle import SPEC_DIR

    frozen = SPEC_DIR / "examples" / "frozen-1.0.0"
    assert json.loads((frozen / "manifest.json").read_text())["schema_version"] == "1.0.0"
    rep = validate(frozen, "semantic")
    assert rep.codes("ERROR") == set() and "UNKNOWN_MINOR" not in rep.codes("WARN"), rep.summary()
    assert not read_streams(frozen / "streams.mcap").get("/ar/detections")


def _break_truncate(b):
    data = (b / "streams.mcap").read_bytes()
    (b / "streams.mcap").write_bytes(data[: len(data) * 2 // 3])
    _refinalize(b)


def _break_sha(b):
    kf = sorted((b / "keyframes").glob("*.jpg"))[0]
    raw = bytearray(kf.read_bytes())
    raw[100] ^= 0xFF
    kf.write_bytes(bytes(raw))


def _break_kf_without_pose(b):
    def m(poses, frames, intr):
        t = frames[2]["t_ns"]
        return [p for p in poses if abs(p["t_ns"] - t) > 200_000_000], frames, intr

    _rewrite_streams(b, m)


def _break_nonmono(b):
    # Write a valid file, then swap two pose messages' timestamps via a fresh Python MCAP.
    from mcap.writer import Writer

    from ckit.bundle import DESC_PATH
    from ckit.bundle.streams import spec_class

    st = read_streams(b / "streams.mcap")
    desc = DESC_PATH.read_bytes()
    with open(b / "streams.mcap", "wb") as f:
        w = Writer(f)
        w.start("capturekit", "test")
        ids = {}
        for topic, msgs in st.messages.items():
            name = type(msgs[0]).DESCRIPTOR.full_name
            sid = w.register_schema(name, "protobuf", desc)
            ids[topic] = w.register_channel(topic, "protobuf", sid)
        for topic in ("/events",):
            if topic not in ids:
                sid = w.register_schema("capturekit.v1.Event", "protobuf", desc)
                ids[topic] = w.register_channel(topic, "protobuf", sid)
        for topic, msgs in st.messages.items():
            msgs = list(msgs)
            if topic == "/pose":
                msgs[5], msgs[6] = msgs[6], msgs[5]
            for m in msgs:
                w.add_message(ids[topic], m.t_ns, m.SerializeToString(), m.t_ns)
        ev = spec_class("capturekit.v1.Event")(t_ns=1, source="t", code="X")
        w.add_message(ids["/events"], 1, ev.SerializeToString(), 1)
        w.finish()
    _refinalize(b)


def _break_nan(b):
    def m(poses, frames, intr):
        poses[4]["p"] = [float("nan"), 0.0, 0.0]
        return poses, frames, intr

    _rewrite_streams(b, m)


def _break_quat(b):
    def m(poses, frames, intr):
        poses[4]["q"] = [0.9, 0.1, 0.0, 0.0]
        return poses, frames, intr

    _rewrite_streams(b, m)


def _break_intrinsics(b):
    _rewrite_streams(b, lambda p, f, i: (p, f, []))


def _break_manifest(b):
    (b / "manifest.json").unlink()


def _break_major(b):
    m = json.loads((b / "manifest.json").read_text())
    m["schema_version"] = "2.0.0"
    (b / "manifest.json").write_text(json.dumps(m))


def _break_minor(b):
    m = json.loads((b / "manifest.json").read_text())
    m["schema_version"] = "1.7.0"
    (b / "manifest.json").write_text(json.dumps(m))


def _break_missing_file(b):
    sorted((b / "keyframes").glob("*.jpg"))[-1].unlink()


def _break_extra_file(b):
    (b / "notes.txt").write_text("x")


CASES = [
    (_break_truncate, "MCAP_TRUNCATED", "ERROR"),
    (_break_sha, "SHA256_MISMATCH", "ERROR"),
    (_break_kf_without_pose, "KEYFRAME_WITHOUT_POSE", "ERROR"),
    (_break_nonmono, "NON_MONOTONIC_TIME", "ERROR"),
    (_break_nan, "NAN_VALUE", "ERROR"),
    (_break_quat, "NON_UNIT_QUATERNION", "ERROR"),
    (_break_intrinsics, "MISSING_INTRINSICS", "ERROR"),
    (_break_manifest, "MISSING_MANIFEST", "ERROR"),
    (_break_major, "UNSUPPORTED_MAJOR", "ERROR"),
    (_break_minor, "UNKNOWN_MINOR", "WARN"),
    (_break_missing_file, "MISSING_FILE", "ERROR"),
    (_break_extra_file, "UNLISTED_FILE", "WARN"),
]


@pytest.mark.parametrize("breaker,code,severity", CASES, ids=[c[1] for c in CASES])
def test_broken_bundle_yields_code(good, tmp_path, breaker, code, severity) -> None:
    b = _copy(good, tmp_path)
    breaker(b)
    rep = validate(b, "semantic")
    assert code in rep.codes(severity), rep.summary()
    if severity == "ERROR":
        assert rep.verdict == "FAIL"


def test_truncation_fuzz_never_crashes(good, tmp_path) -> None:
    b = _copy(good, tmp_path)
    data = (good / "streams.mcap").read_bytes()
    for cut in range(0, len(data), max(1, len(data) // 60)):
        (b / "streams.mcap").write_bytes(data[:cut])
        rep = validate(b, "semantic")
        assert rep.verdict == "FAIL"
        ck.bundle.read_streams(str(b / "streams.mcap"))


# ---- schema 1.2.0: consent and privacy (ADR 0020) -----------------------------------------


def _v12(
    good: Path,
    tmp_path: Path,
    *,
    consent: dict | None = None,
    privacy: dict | None = None,
    redaction: dict | None = None,
    schema: str = "1.2.0",
) -> Path:
    b = _copy(good, tmp_path)
    m = json.loads((b / "manifest.json").read_text())
    m["schema_version"] = schema
    for key, val in (("consent", consent), ("privacy", privacy)):
        if val is not None:
            m[key] = val
    (b / "manifest.json").write_text(json.dumps(m, indent=2))
    if redaction is not None:
        (b / "privacy").mkdir()
        (b / "privacy" / "redaction.json").write_text(json.dumps(redaction))
    _refinalize(b)
    return b


OK_CONSENT = {"version": "v1", "at": "2026-11-17T09:30:00Z"}
FACES = {"redaction_mode": "faces"}


def _red(good: Path, *entries: dict) -> dict:
    return {"version": 1, "mode": "faces", "best_effort": True, "keyframes": list(entries)}


def _kf_entry(good: Path, status: str = "ok") -> dict:
    first = sorted((good / "keyframes").glob("*.jpg"))[0]
    return {"id": 0, "file": f"keyframes/{first.name}", "status": status, "regions": []}


def test_v120_with_consent_and_faces_is_clean(good, tmp_path) -> None:
    b = _v12(
        good, tmp_path, consent=OK_CONSENT, privacy=FACES, redaction=_red(good, _kf_entry(good))
    )
    rep = validate(b, "quality")
    assert rep.codes("ERROR") == set(), rep.summary()
    assert "UNKNOWN_MINOR" not in rep.codes("WARN") and "REDACTION_ENTRY_NO_FILE" not in rep.codes(
        "INFO"
    )
    roles = {f["path"]: f["role"] for f in json.loads((b / "manifest.json").read_text())["files"]}
    assert roles["privacy/redaction.json"] == "redaction"


def test_v120_redaction_none_needs_no_file(good, tmp_path) -> None:
    b = _v12(good, tmp_path, consent=OK_CONSENT, privacy={"redaction_mode": "none"})
    assert validate(b, "semantic").codes("ERROR") == set()


@pytest.mark.parametrize(
    "kw,code",
    [
        ({"privacy": FACES, "redaction": {"keyframes": []}}, "CONSENT_MISSING"),
        (
            {"consent": {"version": "v1", "at": ""}, "privacy": {"redaction_mode": "none"}},
            "CONSENT_MISSING",
        ),
        (
            {"consent": {"at": "2026-11-17T09:30:00Z"}, "privacy": {"redaction_mode": "none"}},
            "CONSENT_MISSING",
        ),
        ({"consent": OK_CONSENT}, "REDACTION_MANIFEST_MISSING"),
        ({"consent": OK_CONSENT, "privacy": FACES}, "REDACTION_MANIFEST_MISSING"),
    ],
    ids=["no-consent", "empty-at", "no-version", "no-privacy", "faces-without-file"],
)
def test_v120_missing_consent_or_redaction_is_error(good, tmp_path, kw, code) -> None:
    b = _v12(good, tmp_path, **kw)
    rep = validate(b, "semantic")
    assert code in rep.codes("ERROR") and rep.verdict == "FAIL", rep.summary()


@pytest.mark.parametrize("schema", ["1.0.0", "1.1.0"])
def test_older_schema_without_consent_still_passes(good, tmp_path, schema) -> None:
    b = _v12(good, tmp_path, schema=schema)
    rep = validate(b, "semantic")
    assert rep.codes("ERROR") == set(), rep.summary()
    assert not rep.codes("ERROR") & {"CONSENT_MISSING", "REDACTION_MANIFEST_MISSING"}


def test_redaction_entry_without_file_is_info_only(good, tmp_path) -> None:
    gone = {"id": 9, "file": "keyframes/999999.jpg", "status": "ok", "regions": [[1, 1, 2, 2]]}
    dropped = {"id": 8, "file": "keyframes/888888.jpg", "status": "dropped_detector_failure"}
    b = _v12(good, tmp_path, consent=OK_CONSENT, privacy=FACES, redaction=_red(good, gone, dropped))
    rep = validate(b, "integrity")
    assert "REDACTION_ENTRY_NO_FILE" in rep.codes("INFO"), rep.summary()
    assert rep.codes("ERROR") == set() and rep.verdict != "FAIL"
    assert (
        sum(i.code == "REDACTION_ENTRY_NO_FILE" for i in rep.issues) == 1
    )  # dropped entry is expected
    assert "999999" in next(i for i in rep.issues if i.code == "REDACTION_ENTRY_NO_FILE").message


def test_cpp_manifest_writer_emits_consent_and_validates(good, tmp_path) -> None:
    b = _copy(good, tmp_path)
    m = json.loads((b / "manifest.json").read_text())
    (b / "privacy").mkdir()
    (b / "privacy" / "redaction.json").write_text(json.dumps(_red(good)))
    meta = {
        "bundle_id": m["bundle_id"],
        "session_id": m["session_id"],
        "platform": m["device"]["platform"],
        "consent_version": "v1",
        "consent_at": "2026-11-17T09:30:00Z",
        "redaction_mode": "faces",
    }
    c = m["clock"]
    topics = {
        "/pose": "poses",
        "/imu/gyro": "imu_gyro",
        "/imu/accel": "imu_accel",
        "/frame_index": "frames",
    }
    counts = {
        "per_topic": {t: m["counts"][k] for t, k in topics.items()},
        "keyframes": m["counts"]["keyframes"],
        "t_start_ns": c["t_start_ns"],
        "t_end_ns": c["t_end_ns"],
    }
    ck.bundle.write_manifest(str(b), meta, counts, ck.bundle.list_files(str(b)))
    out = json.loads((b / "manifest.json").read_text())
    assert out["schema_version"] == "1.2.0"
    assert out["consent"] == OK_CONSENT and out["privacy"] == FACES
    assert validate(b, "semantic").codes("ERROR") == set()


def test_redaction_entry_paths_cannot_escape_keyframes(good, tmp_path) -> None:
    outside = tmp_path / "secret.jpg"
    outside.write_bytes(b"x")  # exists on disk, so an unchecked path would look fine
    entries = [
        {"id": 1, "file": "../secret.jpg", "status": "ok"},
        {"id": 2, "file": str(outside), "status": "ok"},
        {"id": 3, "file": "keyframes/../manifest.json", "status": "ok"},
        _kf_entry(good),
    ]
    b = _v12(good, tmp_path, consent=OK_CONSENT, privacy=FACES, redaction=_red(good, *entries))
    rep = validate(b, "integrity")
    bad = [i for i in rep.issues if i.code == "REDACTION_ENTRY_BAD_PATH"]
    assert len(bad) == 1 and bad[0].severity == "WARN" and bad[0].message.startswith("3 "), (
        rep.summary()
    )
    assert "secret" not in bad[0].message and "manifest" not in bad[0].message
    assert rep.codes("ERROR") == set() and rep.verdict == "WARN"


@pytest.mark.parametrize(
    "at", ["yesterday", "2026-11-17", "2026-13-01T00:00:00Z", "2026-11-17 09:30:00"]
)
def test_v120_consent_time_must_look_like_iso8601(good, tmp_path, at) -> None:
    b = _v12(
        good, tmp_path, consent={"version": "v1", "at": at}, privacy={"redaction_mode": "none"}
    )
    rep = validate(b, "structural")
    assert "CONSENT_MISSING" in rep.codes("ERROR") and rep.verdict == "FAIL", rep.summary()


@pytest.mark.parametrize(
    "at", ["2026-11-17T09:30:00Z", "2026-11-17T09:30:00.123Z", "2026-11-17T11:30:00+02:00"]
)
def test_v120_consent_time_accepts_iso8601_forms(good, tmp_path, at) -> None:
    b = _v12(
        good, tmp_path, consent={"version": "v1", "at": at}, privacy={"redaction_mode": "none"}
    )
    assert "CONSENT_MISSING" not in validate(b, "structural").codes("ERROR")


# ---- schema 1.3.0: derived /guide/* channels (ADR 0028) -----------------------------------

OBJ = (7, 2, 1.5, 0.45, -2.25, 2.0, 0.9, 0.95, -0.5, 0.8, 4)
DOOR = (0, "W2", 3.0, -1.0, 3.0, 0.0, 0.0, 2.0, 0.9)
WIN = (1, "", 0.0, 2.0, 1.0, 2.0, 0.9, 2.0, 0.75)
WALLS = [("W1", 0.0, 0.0, 4.0, 0.0, False), ("W2", 4.0, 0.0, 4.0, 3.0, True)]
CORNERS = [0.0, 0.0, 4.0, 0.0, 4.0, 3.0, 0.0, 3.0]
GAPS = [
    (0, 4.0, 0.0, 4.0, 3.0, 0.75, "window wall"),
    (4, 1.0, 1.0, 2.0, 2.0, 0.5, "behind the sofa"),
]


def _with_guide(good: Path, tmp_path: Path, labels=(OBJ,), openings=(DOOR, WIN), gaps=GAPS) -> Path:
    b = _copy(good, tmp_path)

    def guide(w) -> None:
        assert w.write_labels(100, list(labels), list(openings))
        assert w.write_shell(100, WALLS, CORNERS, True, False, 2.5, True)
        assert w.write_gaps(100, list(gaps))

    _rewrite_streams(b, lambda p, f, i: (p, f, i), guide=guide)
    m = json.loads((b / "manifest.json").read_text())
    m["schema_version"] = "1.3.0"
    m["counts"].update(guide_labels=1, guide_shell=1, guide_gaps=1)
    m["consent"] = {"version": "v1", "at": "2026-11-18T09:30:00Z"}
    m["privacy"] = {"redaction_mode": "none"}
    (b / "manifest.json").write_text(json.dumps(m, indent=2))
    return b


def test_guide_outputs_roundtrip_independent_reader_and_clean_validate(good, tmp_path) -> None:
    b = _with_guide(good, tmp_path)
    st = read_streams(b / "streams.mcap")  # independent protobuf reader
    lab, shl, gap = st.get("/guide/labels")[0], st.get("/guide/shell")[0], st.get("/guide/gaps")[0]
    assert (lab.objects[0].id, lab.objects[0].cls, lab.objects[0].cz) == (7, 2, -2.25)
    assert (lab.openings[0].wall, lab.openings[1].kind, lab.openings[1].y0) == ("W2", 1, 0.9)
    assert [w.id for w in shl.walls] == ["W1", "W2"] and shl.walls[1].inferred
    assert list(shl.corners) == CORNERS and shl.closed and shl.ceiling_h == 2.5
    assert (gap.gaps[1].type, gap.gaps[1].why) == (4, "behind the sofa")
    back = ck.bundle.read_streams(str(b / "streams.mcap"))
    assert back["labels"][0]["objects"][0] == OBJ and back["shell"][0]["corners"] == CORNERS
    assert back["gaps"][0]["gaps"][0] == GAPS[0]
    rep = validate(b, "quality")
    assert rep.codes("ERROR") == set(), rep.summary()
    assert not {
        c for c in rep.codes("WARN") | rep.codes("INFO") if c.startswith(("GUIDE", "LABEL"))
    }
    assert "COUNT_MISMATCH" not in rep.codes("WARN")


def test_guide_output_rules_and_cpp_parity(good, tmp_path) -> None:
    bad_obj = (7, 2, 1.5, 0.45, -2.25, 2.0, 0.0, 0.95, -0.5, 1.5, 4)  # zero height, conf > 1
    orphan = (0, "W9", 3.0, -1.0, 3.0, 0.0, 0.0, 2.0, 0.9)
    nan_gap = (9, 0.0, 0.0, 1.0, 1.0, float("nan"), "")  # unknown type, NaN pri
    b = _with_guide(good, tmp_path, labels=(bad_obj,), openings=(orphan,), gaps=[nan_gap])
    rep = validate(b, "semantic")
    assert "GUIDE_OUTPUT_BAD_VALUE" in rep.codes("WARN"), rep.summary()
    assert "LABEL_WALL_UNKNOWN" in rep.codes("INFO"), rep.summary()
    cpp = json.loads(ck.bundle.validate_semantic(str(b), "complete"))
    codes = {i["code"] for i in cpp["issues"]}
    assert {"GUIDE_OUTPUT_BAD_VALUE", "LABEL_WALL_UNKNOWN"} <= codes


def test_old_minor_bundles_still_validate_and_unstamped_when_no_outputs(good) -> None:
    assert json.loads((good / "manifest.json").read_text())["schema_version"] in ("1.0.0", "1.1.0")
    rep = validate(good, "quality")
    assert rep.codes("ERROR") == set() and "UNKNOWN_MINOR" not in rep.codes("WARN")
    st = ck.bundle.read_streams(str(good / "streams.mcap"))
    assert st["labels"] == [] and st["shell"] == [] and st["gaps"] == []
