"""`ckit simulate-capture`: a simulated phone. Replays a recorded bundle through the real C API
(ctypes), answers keyframe requests by copying the source JPEG, and finalizes (or is killed)."""

from __future__ import annotations

import ctypes as C
import json
import os
import shutil
import signal
import time
from pathlib import Path

import numpy as np

from ckit import capi
from ckit.core import load
from ckit.replay.driver import load_events


def _cfg(extra: dict | None, threaded: bool) -> bytes:
    c = {"threaded": 1 if threaded else 0, "platform": "other", "model": "simulated"}
    c.update(extra or {})
    # Synthetic sessions have no consent screen; schema 1.2.0+ bundles need consent (ADR 0020):
    # 1.3.0 ones (guide outputs, ADR 0028) and the lean 1.4.0 default (no IMU, ADR 0034). So stamp
    # marked placeholders unless the caller gave real keys. Only the legacy shape (IMU recorded, no
    # guide outputs) stays consent-free.
    lean = not float(c.get("record_imu", 0) or 0)
    needs_consent = lean or float(c.get("guide.understanding", 0) or 0) > 0
    if needs_consent and "consent_version" not in c:
        c.update(
            consent_version="synthetic",
            consent_at="1970-01-01T00:00:00Z",
            redaction_mode="none",
        )
    return json.dumps(c).encode()


def _settle(service, quiet_s: float = 0.3, max_s: float = 10.0) -> None:
    """Services events until none arrived for ``quiet_s`` (worker idle), at most ``max_s``."""
    start = quiet_since = time.monotonic()
    while time.monotonic() - start < max_s:
        time.sleep(0.02)
        if service():
            quiet_since = time.monotonic()
        elif time.monotonic() - quiet_since >= quiet_s:
            return


def simulate(
    source: Path,
    out: Path,
    config: dict | None = None,
    threaded: bool = False,
    kill_after_events: int | None = None,
    platform_events: list[tuple[float, dict]] | None = None,
) -> dict:
    """Returns a summary. With kill_after_events the process SIGKILLs itself (no cleanup runs)."""
    import cv2

    L = capi.lib()
    if out.exists():
        shutil.rmtree(out)
    err = C.c_int(0)
    s = L.ck_session_create(str(out).encode(), _cfg(config, threaded), C.byref(err))
    if not s:
        raise RuntimeError(f"ck_session_create failed: {err.value}")
    events, _ = load_events(source)
    events.sort(key=lambda e: (e[0], e[1], e[2]))
    files = {
        f["frame_id"]: f["file"]
        for f in load().bundle.read_streams(str(source / "streams.mcap"))["frames"]
        if f["file"]
    }
    t0 = events[0][0]
    L.ck_session_start(s, t0)
    plat = sorted(platform_events or [], key=lambda x: x[0])
    pi = 0
    buf = C.create_string_buffer(1 << 16)
    stats = {"keyframe_requests": 0, "acked": 0, "full": 0, "expired": 0}

    def service() -> int:
        """Answers pending keyframe requests; returns how many events were handled."""
        handled = 0
        while True:
            n = L.ck_poll_events(s, buf, len(buf))
            if not n:
                return handled
            for line in buf.raw[:n].decode().splitlines():
                handled += 1
                ev = json.loads(line)
                if ev["type"] == "keyframe_request":
                    stats["keyframe_requests"] += 1
                    src = files.get(ev["frame_id"])
                    ok = 0
                    if src:
                        dst = out / ev["file"]
                        tmp = dst.with_suffix(".tmp")
                        shutil.copyfile(source / src, tmp)
                        os.replace(tmp, dst)
                        ok = 1
                    stats["acked"] += ok
                    L.ck_keyframe_written(s, ev["frame_id"], ok)
                elif ev["type"] == "keyframe_expired":
                    stats["expired"] += 1

    n_done = 0
    for t, _rank, _, kind, p in events:
        while pi < len(plat) and t0 + int(plat[pi][0] * 1e9) <= t:
            d = plat[pi][1]
            pl = capi.Platform(
                d.get("thermal", 0), d.get("low_power", 0), d.get("battery", 80.0),
                d.get("charging", 0), d.get("frame_time_ms", 20.0),
            )  # fmt: skip
            L.ck_ingest_platform(s, t0 + int(plat[pi][0] * 1e9), C.byref(pl))
            pi += 1
        st = capi.OK
        if kind == "pose":
            pose = capi.Pose(p["t_ns"], (C.c_double * 3)(*p["p"]), (C.c_double * 4)(*p["q"]),
                             p["state"], p["reason"])  # fmt: skip
            st = L.ck_ingest_pose(s, C.byref(pose), capi.POSE_OPENCV)
        elif kind in ("gyro", "accel"):
            m = capi.Imu(p[0], (C.c_double * 3)(p[1], p[2], p[3]))
            st = getattr(L, f"ck_ingest_{kind}")(s, C.byref(m))
        elif kind == "intrinsics":
            k = capi.Intrinsics(
                p["t_ns"], p["width"], p["height"], p["fx"], p["fy"], p["cx"], p["cy"]
            )
            st = L.ck_ingest_intrinsics(s, C.byref(k))
        elif kind == "frame":
            src = files.get(p["frame_id"])
            img = cv2.imread(str(source / src), cv2.IMREAD_GRAYSCALE) if src else None
            if img is not None:
                img = np.ascontiguousarray(img)
                fr = capi.Frame(p["t_ns"], p["frame_id"], p["exposure_ns"],
                                img.ctypes.data_as(C.POINTER(C.c_uint8)),
                                img.shape[1], img.shape[0], img.shape[1])  # fmt: skip
                st = L.ck_ingest_frame(s, C.byref(fr))
        elif kind == "points":
            arr = (C.c_float * len(p[1]))(*p[1])
            st = L.ck_ingest_points(s, p[0], arr, len(p[1]) // 3)
        elif kind == "planes":
            c3 = C.c_double * 3
            pl = capi.Plane(
                p["t_ns"], str(p["id"]).encode(), p["kind"].encode(),
                c3(*p["center"]), c3(*p["normal"]), c3(*p["axis_u"]),
                p["extent_u"], p["extent_v"],
            )  # fmt: skip
            st = L.ck_ingest_plane(s, C.byref(pl))
        stats["full"] += st == capi.ERR_FULL
        if not threaded:
            L.ck_session_pump(s)
        service()
        n_done += 1
        if kill_after_events is not None and n_done >= kill_after_events:
            os.kill(os.getpid(), signal.SIGKILL)  # no destructors, no flush: a real forced kill
    t_end = events[-1][0]
    L.ck_session_stop(s, t_end)
    if not threaded:
        L.ck_session_pump(s)
    service()
    if threaded:
        # The worker thread is still chewing the queue and flushes the last keyframe window on
        # Stop. A real shell waits for that (SessionManager.finish sleeps 300 ms after stop, then
        # drains events). Polling once and finalizing raced it: the final request was never
        # acknowledged (ENCODE_MISSING; no keyframes at all in about 40 % of runs).
        _settle(service)
    if not threaded:
        L.ck_session_pump(s)
    status = L.ck_session_finalize(s)
    state = capi.STATES[L.ck_session_state(s)]
    pol = capi.Policy()
    L.ck_policy_get(s, C.byref(pol))
    L.ck_session_destroy(s)
    return {"finalize_status": status, "state": state, "final_policy_level": pol.level, **stats}
