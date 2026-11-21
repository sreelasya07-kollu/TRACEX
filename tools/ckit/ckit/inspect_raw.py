"""`ckit inspect-raw`: summary stats and a top-down trajectory plot for a raw session."""

from __future__ import annotations

from pathlib import Path

import numpy as np

from ckit.raw import RawSession, rate_stats


def summarize(s: RawSession) -> dict:
    pose_t = np.array([p["t_ns"] for p in s.poses], dtype=np.int64)
    frame_t = np.array([f["t_ns"] for f in s.frames], dtype=np.int64)
    states: dict[str, int] = {}
    for p in s.poses:
        states[p["tracking"]] = states.get(p["tracking"], 0) + 1
    P = np.array([p["p"] for p in s.poses]) if s.poses else np.zeros((0, 3))
    path_len = float(np.sum(np.linalg.norm(np.diff(P, axis=0), axis=1))) if len(P) > 1 else 0.0
    dur = (pose_t[-1] - pose_t[0]) / 1e9 if len(pose_t) > 1 else 0.0
    out = {
        "session": s.root.name,
        "device": s.device.get("model", "?"),
        "pose_convention": s.device.get("pose_convention", "?"),
        "duration_s": round(dur, 3),
        "path_length_m": round(path_len, 3),
        "poses": rate_stats(pose_t),
        "gyro": rate_stats(s.imu_times("gyro")),
        "accel": rate_stats(s.imu_times("accel")),
        "frames": rate_stats(frame_t),
        "tracking_states": dict(sorted(states.items())),
        "intrinsics_records": len(s.intrinsics),
        "planes_records": len(s.planes),
        "point_records": len(s.points),
    }
    if s.clockcheck:
        out["clock"] = clock_offsets(s)
    return out


def clock_offsets(s: RawSession) -> dict:
    res = {}
    for src in sorted({r["src"] for r in s.clockcheck}):
        d = np.array([r["raw_ns"] - r["mono_ns"] for r in s.clockcheck if r["src"] == src])
        med = float(np.median(d))
        res[src] = {
            "median_offset_ns": int(med),
            "p95_abs_spread_ns": int(np.percentile(np.abs(d - med), 95)),
            "n": len(d),
        }
    return res


def plot(s: RawSession, out_png: Path) -> Path:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    P = np.array([p["p"] for p in s.poses])
    ok = np.array([p["tracking"] == "NORMAL" for p in s.poses])
    fig, ax = plt.subplots(figsize=(6, 6))
    ax.plot(P[:, 0], P[:, 2], "-", lw=0.8, color="#4477aa", label="trajectory")
    if (~ok).any():
        ax.plot(P[~ok, 0], P[~ok, 2], ".", ms=3, color="#cc3311", label="not NORMAL")
    ax.plot(P[0, 0], P[0, 2], "o", color="#228833", label="start")
    ax.set_aspect("equal", adjustable="datalim")
    ax.set_xlabel("x [m]")
    ax.set_ylabel("z [m]")
    ax.set_title(f"{s.root.name} (top-down, Y up)")
    ax.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_png, dpi=110)
    plt.close(fig)
    return out_png
