"""`ckit recon-bench`: offline reconstruction report for a bundle folder, diffable vs another.

Reuses the existing pycolmap paths: free incremental SfM (`registration_rate`) for the registration
rate and pose-seeded triangulation (`report_bundle`) for reprojection error and sparse points. Both
run single-threaded with a fixed seed so two runs on the same bundle give identical numbers.
"""

from __future__ import annotations

import json
import math
import time
from pathlib import Path

SECTORS = 36  # 10 deg yaw sectors, world Y-up
THREADS = 1
SEED = 0
KEYS = (
    "package_bytes",
    "keyframes",
    "registered_pct",
    "reproj_images",
    "mean_reproj_px",
    "sparse_points",
    "coverage_yaw_pct",
)


def package_bytes(bundle: Path) -> int:
    """Sum of sizes of the files the bundle manifest lists (manifest.json itself included).

    Falls back to every file under the folder when there is no readable manifest, so stray work
    files next to a bundle do not change the number.
    """
    try:
        listed = [f["path"] for f in json.loads((bundle / "manifest.json").read_text())["files"]]
    except (OSError, ValueError, KeyError, TypeError):
        return sum(p.stat().st_size for p in bundle.rglob("*") if p.is_file())
    paths = {bundle / "manifest.json", *(bundle / rel for rel in listed)}
    return sum(p.stat().st_size for p in paths if p.is_file())


def yaw_coverage_pct(quats_wxyz: list[list[float]]) -> float:
    """% of 36 yaw sectors that at least one camera looks into (poses only).

    Quaternions are T_world_from_camera [w, x, y, z]; the camera looks down +Z (OpenCV) and the
    world is Y-up, so yaw = atan2(x, z) of the world-space forward vector. Cameras looking within
    ~1 deg of straight up/down have no defined yaw and are skipped.
    """
    held = set()
    for w, x, y, z in quats_wxyz:
        n = math.sqrt(w * w + x * x + y * y + z * z)
        w, x, y, z = w / n, x / n, y / n, z / n
        fx = 2.0 * (x * z + w * y)  # third column of the rotation matrix = R @ [0, 0, 1]
        fz = 1.0 - 2.0 * (x * x + y * y)
        if math.hypot(fx, fz) < 0.0175:
            continue
        deg = math.degrees(math.atan2(fx, fz)) % 360.0
        held.add(int(deg // (360.0 / SECTORS)) % SECTORS)
    return round(100.0 * len(held) / SECTORS, 2)


def recon_bench(bundle: Path) -> dict:
    import pycolmap

    from ckit.convert.colmap import load_keyframe_poses
    from ckit.convert.reprojection import report_bundle
    from ckit.e2e import _keyframe_files
    from ckit.quality_report import registration_rate

    bundle = Path(bundle)
    t0 = time.perf_counter()
    files = _keyframe_files(bundle)
    if len(files) < 3:
        raise ValueError(f"{bundle.name}: only {len(files)} keyframes; need >= 3")
    pycolmap.set_random_seed(SEED)
    reg = registration_rate(bundle, files, num_threads=THREADS)
    pycolmap.set_random_seed(SEED)
    rep = report_bundle(bundle, num_threads=THREADS)
    want = set(files)
    quats = [f["q"] for f in load_keyframe_poses(bundle)["frames"] if f["file"] in want]
    return {
        "bundle": bundle.name,
        "package_bytes": package_bytes(bundle),
        "keyframes": len(files),
        "registered_pct": round(100.0 * reg["registration_rate"], 2),
        "reproj_images": rep["images"],
        "mean_reproj_px": rep["reproj_mean_px"],
        "sparse_points": rep["points3D"],
        "coverage_yaw_pct": yaw_coverage_pct(quats),
        "seconds": round(time.perf_counter() - t0, 1),
    }


def stable(rep: dict) -> dict:
    """The run-to-run comparable part of a report (wall-clock `seconds` removed)."""
    return {k: v for k, v in rep.items() if k != "seconds"}


def delta(rep: dict, base: dict, keys: tuple[str, ...] = KEYS) -> dict:
    """rep - baseline per numeric key (None when either side has no value)."""
    out = {}
    for k in keys:
        a, b = rep.get(k), base.get(k)
        out[k] = None if a is None or b is None else round(a - b, 4)
    return out


def table(rep: dict, base: dict | None = None) -> str:
    keys = (*KEYS, "seconds")
    if base is None:
        rows = [("key", rep["bundle"])] + [(k, str(rep[k])) for k in keys]
    else:
        d = delta(rep, base, keys)
        rows = [("key", rep["bundle"], "baseline " + base["bundle"], "delta")] + [
            (k, str(rep[k]), str(base[k]), "n/a" if d[k] is None else f"{d[k]:+g}") for k in keys
        ]
    widths = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    return "\n".join("  ".join(c.ljust(w) for c, w in zip(r, widths, strict=True)) for r in rows)
