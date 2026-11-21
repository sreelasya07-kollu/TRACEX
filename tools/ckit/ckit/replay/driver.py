"""`ckit replay`: feed a Scan Bundle through C++ core sinks in t_ns order.

Ordering: merged by (t_ns, channel rank, original index). Rank (tie-break) is
intrinsics < pose < gyro < accel < frame < event < points < planes (docs/replay.md).
Faults are injected deterministically from --seed. The report contains no wall-clock values, so two
identical runs give byte-identical JSON.
"""

from __future__ import annotations

import json
import time
from collections import OrderedDict
from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np

from ckit.core import load

RANK = {
    "intrinsics": 0,
    "pose": 1,
    "gyro": 2,
    "accel": 3,
    "frame": 4,
    "event": 5,
    "points": 6,
    "planes": 7,
    "detections": 8,
}
NOT_AVAILABLE, REASON_OTHER = 3, 8
THERMAL_STATES = ("nominal", "fair", "serious", "critical")
REPORT_SCHEMA = "ckit.replay/1"


@dataclass
class Faults:
    drop_frames: float = 0.0  # probability of dropping each frame
    jitter_ms: float = 0.0  # uniform +-jitter on frame/IMU t_ns, delivered in original order
    tracking_loss: list[tuple[float, float]] = field(default_factory=list)  # seconds from start
    thermal: list[tuple[float, str]] = field(default_factory=list)  # (seconds, state)
    seed: int = 0

    @staticmethod
    def parse_window(s: str) -> tuple[float, float]:
        a, b = s.split(":")
        t0, t1 = float(a), float(b)
        if t1 <= t0:
            raise ValueError(f"tracking-loss window must have T1 > T0: {s}")
        return t0, t1

    @staticmethod
    def parse_thermal(s: str) -> tuple[float, str]:
        a, b = s.split(":")
        if b not in THERMAL_STATES:
            raise ValueError(f"thermal state must be one of {THERMAL_STATES}: {s}")
        return float(a), b


class SystemClock:
    def now_ns(self) -> int:
        return time.monotonic_ns()

    def sleep_until(self, t_ns: int) -> None:
        d = t_ns - self.now_ns()
        if d > 0:
            time.sleep(d / 1e9)


class FakeClock:
    """Injectable clock for tests: sleeping advances time instantly and is recorded."""

    def __init__(self) -> None:
        self.t = 0
        self.sleeps = 0

    def now_ns(self) -> int:
        return self.t

    def sleep_until(self, t_ns: int) -> None:
        if t_ns > self.t:
            self.t = t_ns
            self.sleeps += 1


class FrameCache:
    """Small LRU of decoded luma images (keyframe JPEGs)."""

    def __init__(self, root: Path, size: int = 8) -> None:
        self.root, self.size = root, size
        self.cache: OrderedDict[str, np.ndarray | None] = OrderedDict()
        self.hits = self.misses = 0

    def get(self, file: str) -> np.ndarray | None:
        if not file:
            return None
        if file in self.cache:
            self.hits += 1
            self.cache.move_to_end(file)
            return self.cache[file]
        import cv2

        self.misses += 1
        img = cv2.imread(str(self.root / file), cv2.IMREAD_GRAYSCALE)
        img = np.ascontiguousarray(img) if img is not None else None
        self.cache[file] = img
        if len(self.cache) > self.size:
            self.cache.popitem(last=False)
        return img


def load_events(bundle: Path) -> tuple[list[tuple], dict]:
    """All bundle records as (t_ns, rank, idx, kind, payload), unsorted."""
    ck = load()
    s = ck.bundle.read_streams(str(bundle / "streams.mcap"))
    ev: list[tuple] = []
    for i, k in enumerate(s["intrinsics"]):
        ev.append((k["t_ns"], RANK["intrinsics"], i, "intrinsics", k))
    for i, p in enumerate(s["poses"]):
        ev.append((p["t_ns"], RANK["pose"], i, "pose", p))
    for kind in ("gyro", "accel"):
        for i, g in enumerate(s[kind]):
            ev.append((g[0], RANK[kind], i, kind, g))
    for i, f in enumerate(s["frames"]):
        ev.append((f["t_ns"], RANK["frame"], i, "frame", f))
    for i, e in enumerate(s["events"]):
        ev.append((e[0], RANK["event"], i, "event", e))
    for i, p in enumerate(s["points"]):
        ev.append((p[0], RANK["points"], i, "points", p))
    for i, p in enumerate(s["planes"]):
        ev.append((p["t_ns"], RANK["planes"], i, "planes", p))
    for i, d in enumerate(s.get("detections", [])):
        ev.append((d["t_ns"], RANK["detections"], i, "detections", d))
    meta = {"finalized": s["finalized"], "undecodable": s["undecodable"]}
    return ev, meta


def inject(events: list[tuple], f: Faults) -> tuple[list[tuple], dict]:
    """Apply faults. Returns delivery-ordered events and injection counts."""
    events = sorted(events, key=lambda e: (e[0], e[1], e[2]))
    if not events:
        return events, {}
    rng = np.random.Generator(np.random.PCG64(f.seed))
    t0 = events[0][0]
    out: list[tuple] = []
    n = {"frames_dropped": 0, "jittered": 0, "poses_lost": 0, "thermal_events": 0}
    windows = [(t0 + int(a * 1e9), t0 + int(b * 1e9)) for a, b in f.tracking_loss]
    for t, rank, idx, kind, p in events:
        if kind == "frame" and f.drop_frames > 0 and rng.random() < f.drop_frames:
            n["frames_dropped"] += 1
            continue
        if kind in ("frame", "gyro", "accel") and f.jitter_ms > 0:
            dt = int(round(rng.uniform(-f.jitter_ms, f.jitter_ms) * 1e6))
            if dt:
                n["jittered"] += 1
                t = t + dt
                p = dict(p, t_ns=t) if isinstance(p, dict) else (t, *p[1:])
        if kind == "pose" and any(a <= t <= b for a, b in windows):
            p = dict(p, state=NOT_AVAILABLE, reason=REASON_OTHER)
            n["poses_lost"] += 1
        out.append((t, rank, idx, kind, p))
    extra = []
    for k, (a, b) in enumerate(windows):
        attrs = [("t1_ns", str(b))]
        extra.append(
            (a, RANK["event"], -1000 - k, "event", (a, "replay", "inject.tracking_loss", "", attrs))
        )
    for k, (ts, state) in enumerate(f.thermal):
        ta = t0 + int(ts * 1e9)
        extra.append(
            (
                ta,
                RANK["event"],
                -2000 - k,
                "event",
                (ta, "platform", "thermal", "", [("state", state)]),
            )
        )
        n["thermal_events"] += 1
    if extra:
        # Injected events are placed by their own t_ns in the (possibly jittered) delivery order.
        merged, ei = [], sorted(extra, key=lambda e: (e[0], e[2]))
        j = 0
        for e in out:
            while j < len(ei) and (ei[j][0], ei[j][1]) <= (e[0], e[1]):
                merged.append(ei[j])
                j += 1
            merged.append(e)
        merged.extend(ei[j:])
        out = merged
    return out, n


def dispatch(sink, kind: str, p, luma) -> None:
    if kind == "pose":
        sink.on_pose(p["t_ns"], p["p"], p["q"], p["state"], p["reason"])
    elif kind == "gyro":
        sink.on_gyro(*p)
    elif kind == "accel":
        sink.on_accel(*p)
    elif kind == "intrinsics":
        sink.on_intrinsics(
            p["t_ns"],
            p["width"],
            p["height"],
            p["fx"],
            p["fy"],
            p["cx"],
            p["cy"],
            p["model"],
            p["distortion"],
        )
    elif kind == "frame":
        sink.on_frame(p["t_ns"], p["frame_id"], p["file"], p["keyframe"], p["exposure_ns"], luma)
    elif kind == "event":
        sink.on_event(p[0], p[1], p[2], p[3], list(p[4]))
    elif kind == "points":
        sink.on_points(p[0], list(p[1]))
    elif kind == "detections":
        sink.on_detections(p["t_ns"], p["frame_id"], [tuple(b) for b in p["boxes"]])
    elif kind == "planes":
        sink.on_plane(
            p["t_ns"],
            p["id"],
            p["kind"],
            p["center"],
            p["normal"],
            p["axis_u"],
            p["extent_u"],
            p["extent_v"],
        )


def _round(o, nd: int = 6):
    if isinstance(o, float):
        return round(o, nd)
    if isinstance(o, dict):
        return {k: _round(v, nd) for k, v in o.items()}
    if isinstance(o, list):
        return [_round(v, nd) for v in o]
    return o


def replay(
    bundle: Path,
    sinks: list[str] | None = None,
    faults: Faults | None = None,
    realtime: bool = False,
    speed: float = 1.0,
    clock=None,
    sink_config: dict[str, dict] | None = None,
) -> dict:
    ck = load()
    faults = faults or Faults()
    sinks = sinks or ["metrics"]
    cfg = sink_config or {}
    objs = [ck.replay.Sink(n, json.dumps(cfg.get(n, {}), sort_keys=True)) for n in sinks]
    events, meta = load_events(bundle)
    events, injected = inject(events, faults)
    want_images = any(s.needs_images for s in objs)
    cache = FrameCache(bundle)
    clock = clock or SystemClock()
    wall0 = clock.now_ns()
    t_first = events[0][0] if events else 0
    counts: dict[str, int] = {}
    for t, _, _, kind, p in events:
        if realtime:
            clock.sleep_until(wall0 + int((t - t_first) / speed))
        counts[kind] = counts.get(kind, 0) + 1
        luma = cache.get(p["file"]) if (kind == "frame" and want_images) else None
        for s in objs:
            dispatch(s, kind, p, luma)
    t_end = max((e[0] for e in events), default=0)
    for s in objs:
        s.on_end(t_end)
    report = {
        "schema": REPORT_SCHEMA,
        "bundle": bundle.name,
        "options": {
            "sinks": sinks,
            "faults": asdict(faults),
            "mode": "realtime" if realtime else "fast",
            "sink_config": cfg,
        },
        "input": {"delivered": counts, "injected": injected, **meta},
        "sinks": {s.name: json.loads(s.report_json()) for s in objs},
    }
    return _round(json.loads(json.dumps(report)))


def dumps(report: dict) -> str:
    return json.dumps(report, indent=2, sort_keys=True) + "\n"
