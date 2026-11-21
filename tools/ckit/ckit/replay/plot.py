"""Top-down trajectory viewer: X/Z path coloured by tracking state, keyframes marked."""

from __future__ import annotations

from pathlib import Path

from ckit.core import load

COLORS = {1: "tab:green", 2: "tab:orange", 3: "tab:red", 0: "tab:gray"}


def plot(bundle: Path, out: Path) -> Path:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    s = load().bundle.read_streams(str(bundle / "streams.mcap"))
    poses = s["poses"]
    fig, ax = plt.subplots(figsize=(6, 6))
    for a, b in zip(poses, poses[1:], strict=False):
        ax.plot(
            [a["p"][0], b["p"][0]], [a["p"][2], b["p"][2]], color=COLORS.get(a["state"], "k"), lw=1
        )
    kf_t = {f["t_ns"] for f in s["frames"] if f["keyframe"]}
    kx = [p["p"][0] for p in poses if p["t_ns"] in kf_t]
    kz = [p["p"][2] for p in poses if p["t_ns"] in kf_t]
    ax.scatter(kx, kz, s=6, c="tab:blue", label="keyframe pose", zorder=3)
    ax.set_aspect("equal")
    ax.set_xlabel("X (m)")
    ax.set_ylabel("Z (m)")
    ax.set_title(f"{bundle.name}: green=normal, orange=limited, red=lost")
    ax.legend(loc="best")
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=120, bbox_inches="tight")
    plt.close(fig)
    return out
