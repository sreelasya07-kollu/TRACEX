"""ctypes binding of the C API (core/include/capturekit/capi/capturekit.h): the same ABI the
Swift and Kotlin shells use. Used by `ckit simulate-capture` and the crash-safety tests."""

from __future__ import annotations

import ctypes as C
import os
from pathlib import Path

from ckit.core import CAPTUREKIT_ROOT

OK, ERR_STATE, ERR_ARG, ERR_IO, ERR_POLICY, ERR_FULL = range(6)
STATES = ["IDLE", "CALIBRATING", "SCANNING", "PAUSED", "FINALIZING", "COMPLETE", "FAILED"]
POSE_OPENCV, POSE_ARKIT_GL = 0, 1


class Pose(C.Structure):
    _fields_ = [
        ("t_ns", C.c_int64),
        ("t", C.c_double * 3),
        ("q_wxyz", C.c_double * 4),
        ("tracking_state", C.c_int32),
        ("tracking_reason", C.c_int32),
    ]


class Imu(C.Structure):
    _fields_ = [("t_ns", C.c_int64), ("v", C.c_double * 3)]


class Intrinsics(C.Structure):
    _fields_ = [
        ("t_ns", C.c_int64),
        ("width", C.c_uint32),
        ("height", C.c_uint32),
        ("fx", C.c_double),
        ("fy", C.c_double),
        ("cx", C.c_double),
        ("cy", C.c_double),
    ]


class Frame(C.Structure):
    _fields_ = [
        ("t_ns", C.c_int64),
        ("frame_id", C.c_uint32),
        ("exposure_ns", C.c_int64),
        ("luma", C.POINTER(C.c_uint8)),
        ("width", C.c_int32),
        ("height", C.c_int32),
        ("stride", C.c_int32),
    ]


class Platform(C.Structure):
    _fields_ = [
        ("thermal_level", C.c_int32),
        ("low_power_mode", C.c_int32),
        ("battery_pct", C.c_double),
        ("charging", C.c_int32),
        ("frame_time_ms", C.c_double),
    ]


class Plane(C.Structure):
    _fields_ = [
        ("t_ns", C.c_int64),
        ("id", C.c_char_p),
        ("kind", C.c_char_p),
        ("center", C.c_double * 3),
        ("normal", C.c_double * 3),
        ("axis_u", C.c_double * 3),
        ("extent_u", C.c_double),
        ("extent_v", C.c_double),
    ]


class Detection(C.Structure):
    _fields_ = [
        ("cls", C.c_int32),
        ("score", C.c_float),
        ("x0", C.c_float),
        ("y0", C.c_float),
        ("x1", C.c_float),
        ("y1", C.c_float),
    ]


class Policy(C.Structure):
    _fields_ = [
        ("level", C.c_int32),
        ("keyframe_rate_scale", C.c_double),
        ("ml_rate_scale", C.c_double),
        ("capture_resolution_tier", C.c_int32),
        ("overlay_enabled", C.c_int32),
        ("upload_allowed", C.c_int32),
        ("must_pause", C.c_int32),
    ]


def _lib_path() -> Path:
    d = os.environ.get("CKIT_CORE_DIR") or str(CAPTUREKIT_ROOT / "build" / "core-py" / "py")
    return Path(d) / "libcapturekit_capi.so"


def _load() -> C.CDLL:
    p = _lib_path()
    if not p.exists():
        raise ImportError(f"{p} not found. Build it with: capturekit/scripts/build_core_py.sh")
    lib = C.CDLL(str(p))
    S = C.c_void_p
    lib.ck_version.restype = C.c_char_p
    lib.ck_session_create.restype = S
    lib.ck_session_create.argtypes = [C.c_char_p, C.c_char_p, C.POINTER(C.c_int)]
    lib.ck_session_destroy.argtypes = [S]
    for n in ("start", "pause", "resume", "stop"):
        f = getattr(lib, f"ck_session_{n}")
        f.argtypes = [S, C.c_int64]
        f.restype = C.c_int
    for n in ("finalize", "pump"):
        f = getattr(lib, f"ck_session_{n}")
        f.argtypes = [S]
        f.restype = C.c_int
    lib.ck_session_state.argtypes = [S]
    lib.ck_session_state.restype = C.c_int
    lib.ck_ingest_pose.argtypes = [S, C.POINTER(Pose), C.c_int32]
    for n, t in (
        ("gyro", Imu),
        ("accel", Imu),
        ("intrinsics", Intrinsics),
        ("frame", Frame),
        ("plane", Plane),
    ):
        getattr(lib, f"ck_ingest_{n}").argtypes = [S, C.POINTER(t)]
    lib.ck_ingest_points.argtypes = [S, C.c_int64, C.POINTER(C.c_float), C.c_size_t]
    lib.ck_ingest_platform.argtypes = [S, C.c_int64, C.POINTER(Platform)]
    lib.ck_ingest_detections.argtypes = [S, C.c_int64, C.c_uint32, C.POINTER(Detection), C.c_size_t]
    lib.ck_keyframe_written.argtypes = [S, C.c_uint32, C.c_int32]
    lib.ck_poll_events.argtypes = [S, C.c_char_p, C.c_size_t]
    lib.ck_poll_events.restype = C.c_size_t
    lib.ck_policy_get.argtypes = [S, C.POINTER(Policy)]
    lib.ck_recover.argtypes = [C.c_char_p, C.c_char_p, C.c_size_t]
    for n in (
        "pose",
        "gyro",
        "accel",
        "intrinsics",
        "frame",
        "points",
        "plane",
        "platform",
        "detections",
    ):
        getattr(lib, f"ck_ingest_{n}").restype = C.c_int
    lib.ck_keyframe_written.restype = C.c_int
    lib.ck_policy_get.restype = C.c_int
    lib.ck_recover.restype = C.c_int
    return lib


_lib: C.CDLL | None = None


def lib() -> C.CDLL:
    global _lib
    if _lib is None:
        _lib = _load()
    return _lib


def recover(bundle: Path) -> tuple[int, dict]:
    import json

    buf = C.create_string_buffer(1024)
    st = lib().ck_recover(str(bundle).encode(), buf, len(buf))
    return st, json.loads(buf.value.decode() or "{}")
