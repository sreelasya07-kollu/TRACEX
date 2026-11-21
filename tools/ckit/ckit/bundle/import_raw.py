"""`ckit import-raw`: Phase 0 raw session -> Scan Bundle, written by the C++ core writer.

Pose conversion to our convention (OpenCV camera) happens here, once, at the edge.
Until Phase 2 lands, the ARKit/ARCore flip is the one-line rule in conventions.md:
T_W_C = T_W_Cplatform * diag(1,-1,-1).
"""

from __future__ import annotations

import bisect
import json
import shutil
import uuid
from pathlib import Path

import numpy as np

from ckit import geometry
from ckit.core import load
from ckit.raw import RawSession

STATE = {"NORMAL": 1, "LIMITED": 2, "NOT_AVAILABLE": 3}
REASON = {
    "NONE": 0,
    "INITIALIZING": 1,
    "EXCESSIVE_MOTION": 2,
    "INSUFFICIENT_FEATURES": 3,
    "INSUFFICIENT_LIGHT": 4,
    "RELOCALIZING": 5,
    "BAD_STATE": 6,
    "CAMERA_UNAVAILABLE": 7,
}


def to_internal_pose(p, q, convention: str) -> tuple[list[float], list[float]]:
    """Platform pose -> T_world_from_camera (OpenCV camera axes), q as [w,x,y,z]."""
    if convention == "opencv":
        return list(p), list(q)
    if convention in ("arkit", "arcore", "opengl"):
        # Raw recorders store q as [w,x,y,z] for every platform (recording-protocol.md).
        t, qq = geometry.from_arkit(p, q)
        return list(t), list(qq)
    raise ValueError(f"unknown pose_convention {convention!r}")


def import_raw(raw_dir: Path, out: Path, keyframe_filter=None, quality: dict | None = None) -> dict:
    """Converts a raw session. keyframe_filter(frame_dict) -> bool selects kept frames.

    quality: optional {"frames": {frame_id: per-frame record}, "config": {...}, "config_hash",
    "config_version"} from the core quality sink: metrics, reasons and keyframe decisions are
    written to /frame_index, the config hash to the manifest and a QUALITY_CONFIG event.
    """
    qf = (quality or {}).get("frames", {})
    ck = load()
    s = RawSession.load(raw_dir)
    conv = s.device.get("pose_convention", "opencv")
    off = int(s.device.get("clock_offset_ns", 0))
    if out.exists():
        shutil.rmtree(out)
    (out / "keyframes").mkdir(parents=True)
    w = ck.bundle.StreamWriter()
    if not w.open(str(out / "streams.mcap")):
        raise RuntimeError(f"cannot open {out / 'streams.mcap'}")

    # Merge everything by t_ns so the writer sees one time-ordered stream (stable tie-break:
    # intrinsics, pose, gyro, accel, frame, points, planes).
    ev: list[tuple[int, int, str, object]] = []
    for k in s.intrinsics:
        ev.append((k["t_ns"] + off, 0, "intr", k))
    for p in s.poses:
        ev.append((p["t_ns"] + off, 1, "pose", p))
    for r in s.imu:
        ev.append((r["t_ns"] + off, 2 if r["type"] == "gyro" else 3, r["type"], r))
    for f in s.frames:
        ev.append((f["t_ns"] + off, 4, "frame", f))
    for r in s.points:
        ev.append((r["t_ns"] + off, 5, "points", r))
    for r in s.planes:
        ev.append((r["t_ns"] + off, 6, "plane", r))
    ev.sort(key=lambda e: (e[0], e[1]))

    pose_t = [p["t_ns"] + off for p in s.poses]
    kept = 0
    last_plane_t: dict[str, int] = {}
    for t, _, kind, r in ev:
        if kind == "intr":
            w.write_intrinsics(t, r["w"], r["h"], r["fx"], r["fy"], r["cx"], r["cy"])
        elif kind == "pose":
            p, q = to_internal_pose(r["p"], r["q"], conv)
            w.write_pose(t, p, q, STATE.get(r["tracking"], 0), REASON.get(r.get("reason"), 8))
        elif kind == "gyro":
            w.write_gyro(t, *r["v"])
        elif kind == "accel":
            w.write_accel(t, *r["v"])
        elif kind == "frame":
            i = bisect.bisect_left(pose_t, t)
            near = min(
                (j for j in (i - 1, i) if 0 <= j < len(pose_t)),
                key=lambda j: abs(pose_t[j] - t),
                default=None,
            )
            state = STATE.get(s.poses[near]["tracking"], 0) if near is not None else 0
            q = qf.get(r["i"])
            keep = keyframe_filter(r) if keyframe_filter else (q["keyframe"] if q else True)
            rec = {
                "t_ns": t,
                "frame_id": r["i"],
                "exposure_ns": r.get("exposure_ns", 0),
                "tracking_state": state,
                "keyframe": keep,
            }
            if q:
                for k in (
                    "sharpness",
                    "sharpness_norm",
                    "blur_risk_px",
                    "clip_low",
                    "clip_high",
                    "corners",
                    "corner_spread",
                    "quality",
                    "reasons",
                ):
                    v = q.get(k)
                    if v is not None:
                        rec[k] = v
            if keep:
                name = f"keyframes/{kept:06d}.jpg"
                shutil.copyfile(s.root / r["file"], out / name)
                rec["file"] = name
                kept += 1
            w.write_frame(rec)
        elif kind == "points":
            w.write_points(t, np.asarray(r["pts"], np.float32).ravel().tolist())
        elif kind == "plane":
            # Several anchors update in one AR frame, but t_ns must be strictly increasing per
            # channel: later anchors in the same frame get +1 ns each (documented in SPEC.md).
            tp = max(t, last_plane_t.get("_", -1) + 1)
            last_plane_t["_"] = tp
            w.write_plane({**r, "t_ns": tp, "id": str(r["id"])})
    t0 = ev[0][0] if ev else 0
    w.write_event(
        t0,
        "platform",
        "SESSION_IMPORTED",
        f"import-raw from {raw_dir.name}",
        [("pose_convention", conv), ("frames", str(len(s.frames)))],
    )
    if quality:
        w.write_event(
            t0 + 1,  # events are strictly increasing per channel
            "quality",
            "QUALITY_CONFIG",
            "keyframe selection config",
            [("version", quality["config_version"]), ("hash", quality["config_hash"])],
        )
    counts = w.close()

    k0 = s.intrinsics[0]
    meta = {
        "bundle_id": str(uuid.uuid5(uuid.NAMESPACE_URL, f"ckit:{raw_dir.resolve().name}")),
        "session_id": raw_dir.name,
        "platform": s.device.get("platform", "other"),
        "model": s.device.get("model", ""),
        "os": s.device.get("os", ""),
        "app_version": s.device.get("app_version", "0.0.1"),
        "clock_base": s.device.get("clock_base", "unknown"),
        "clock_offset_ns": off,
        "q_camera_from_imu": s.device.get("q_camera_from_imu", [1, 0, 0, 0]),
        "capture_settings_json": json.dumps(
            {
                "source": "import-raw",
                "pose_convention": conv,
                **(
                    {
                        "quality_config": quality["config"],
                        "quality_config_hash": quality["config_hash"],
                        "quality_config_version": quality["config_version"],
                    }
                    if quality
                    else {}
                ),
            },
            sort_keys=True,
        ),
    }
    ck.bundle.write_camera_json(
        str(out), k0["w"], k0["h"], k0["fx"], k0["fy"], k0["cx"], k0["cy"], 0, [], meta
    )
    files = ck.bundle.list_files(str(out))
    ck.bundle.write_manifest(str(out), meta, counts, files)
    return counts


def import_raw_selected(raw_dir: Path, out: Path, config: dict | None = None) -> dict:
    """Two passes: import all frames to a temp bundle, run the core quality sink over it, then
    import again keeping only the selected keyframes (with per-frame metrics and reasons)."""
    import tempfile

    from ckit.replay.driver import replay

    with tempfile.TemporaryDirectory(prefix="ckit_sel_") as td:
        tmp = Path(td) / "all"
        import_raw(raw_dir, tmp)
        cfg = dict(config or {}, emit_frames=1)
        rep = replay(tmp, ["quality"], sink_config={"quality": cfg})["sinks"]["quality"]
        frames = {}
        for fr in rep["per_frame"]:
            frames[fr["frame_id"]] = fr
    # Frame ids in the temp bundle are the raw frame indices ("i").
    q = {
        "frames": frames,
        "config": rep["config"],
        "config_hash": rep["config_hash"],
        "config_version": rep["config_version"],
    }
    counts = import_raw(raw_dir, out, quality=q)
    counts["selected_keyframes"] = rep["keyframes"]
    return counts
