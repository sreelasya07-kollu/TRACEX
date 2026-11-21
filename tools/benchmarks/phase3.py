"""Phase 3 benchmark runner (Step B/C/E/F/G). One entry point, cached by parameter hash.

  python -m benchmarks.phase3 prepare --scene room0          # Phase 1+2 workspace
  python -m benchmarks.phase3 run --scene room0 --runs R0 R1 R2 R3 R4 R5 R6
Results: runs/bench_phase3/<scene>/<run>/{phase3/, eval.json}; summary in summary.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from dataclasses import replace
from pathlib import Path

from eval.phase3_eval import evaluate_phase3
from spatial.io.ingest import ingest
from spatial.phase3 import Phase3Config, run_phase3
from spatial.sfm.refine import refine

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / "runs" / "bench_phase3"

# Fixed dataset subsets (recorded in workspace_meta.json / meta.json frame_ids).
SCENES = {
    "room0": dict(
        dataset="replica",
        src=ROOT / "data/replica/room0",
        stride=4,
        max_frames=100,
        downscale=2,
        prior="gt_noisy",
        noise_preset="vio_small",
    ),
    "office0": dict(
        dataset="replica",
        src=ROOT / "data/replica/office0",
        stride=4,
        max_frames=100,
        downscale=2,
        prior="gt_noisy",
        noise_preset="vio_small",
    ),
    # Full-scale configs for the big machine (docs/heavy-compute/inventory.yaml). Need
    # REPLICA_STRIDE=1 data/fetch.sh replica (all 2000 frames) and >= 32 GB RAM.
    "room0_full": dict(
        dataset="replica",
        src=ROOT / "data/replica/room0",
        stride=1,
        max_frames=None,
        downscale=1,
        prior="gt_noisy",
        noise_preset="vio_small",
        n_gt=1_000_000,
        phase3_downscale=1,
    ),
    "office0_full": dict(
        dataset="replica",
        src=ROOT / "data/replica/office0",
        stride=1,
        max_frames=None,
        downscale=1,
        prior="gt_noisy",
        noise_preset="vio_small",
        n_gt=1_000_000,
        phase3_downscale=1,
    ),
    "tum_desk": dict(
        dataset="tum",
        src=ROOT / "data/tum/rgbd_dataset_freiburg1_desk",
        stride=5,
        max_frames=100,
        downscale=1,
        prior="gt_noisy",
        noise_preset="vio_small",
    ),
}

BASE = Phase3Config(downscale=2, max_frames=None)
RUNS = {
    "R0": replace(BASE, pose_source="gt", depth_source="sensor", masks=False, consistency=False),
    "R1": replace(BASE, depth_source="sensor", masks=True, consistency=True),
    "R2": replace(BASE, depth_source="mono", align=False, masks=False, consistency=False),
    "R3": replace(BASE, depth_source="mono", align=True, masks=False, consistency=False),
    "R4": replace(BASE, depth_source="mono", align=True, masks=True, consistency=False),
    "R5": replace(BASE, depth_source="mono", align=True, masks=True, consistency=True),
    "R6": replace(BASE, depth_source="sensor", masks=True, consistency=True),
    # Phase 3 completion: no depth sensor, OpenMVS multi-view depth (CPU docker)
    "R7": replace(BASE, depth_source="mvs", masks=True, consistency=True),
    "R8": replace(BASE, depth_source="hybrid", masks=True, consistency=True),
}


def _hash(obj) -> str:
    obj = dict(obj)
    if obj.get("depth_source") not in ("mvs", "hybrid"):  # keep caches of pre-MVS runs valid
        obj.pop("mvs_p", None)
        obj.pop("mvs_conf_min", None)
    ap = obj.get("align_p")
    if ap is not None and getattr(ap, "method", None) == "ransac":
        # keep caches valid: hash as the pre-`method` AlignParams repr did
        obj["align_p"] = (
            f"AlignParams(ransac_iters={ap.ransac_iters!r}, inlier_rel={ap.inlier_rel!r}, "
            f"min_inliers={ap.min_inliers!r}, max_residual_rel={ap.max_residual_rel!r}, "
            f"huber_rel={ap.huber_rel!r}, smooth_window={ap.smooth_window!r}, seed={ap.seed!r})"
        )
    return hashlib.sha256(json.dumps(obj, sort_keys=True, default=str).encode()).hexdigest()[:16]


def prepare(scene: str) -> Path:
    s = SCENES[scene]
    run = BENCH / scene / "_workspace"
    if (run / "workspace" / "poses_refined.json").exists():
        return run / "workspace"
    ws = ingest(
        s["dataset"],
        s["src"],
        run,
        max_frames=s["max_frames"],
        stride=s["stride"],
        downscale=s["downscale"],
        prior=s["prior"],
        noise_preset=s["noise_preset"],
        seed=0,
    )
    refine(ws, run, max_image_size=1024)
    return ws


def run(scene: str, names: list[str], overrides: dict | None = None, tag: str = "") -> dict:
    ws = prepare(scene)
    summary_p = BENCH / scene / "summary.json"
    summary = json.loads(summary_p.read_text()) if summary_p.exists() else {}
    cache: dict = {}
    for name in names:
        sc = SCENES[scene]
        cfg = replace(RUNS[name], downscale=sc.get("phase3_downscale", RUNS[name].downscale))
        ov = dict(overrides or {})
        for k in [k for k in ov if k.endswith("_p") and isinstance(ov[k], dict)]:
            ov[k] = replace(getattr(cfg, k), **ov[k])  # nested dataclass overrides
        cfg = replace(cfg, **ov)
        key = name + tag
        out = BENCH / scene / key / "phase3"
        h = _hash(vars(cfg))
        ev_p = out.parent / "eval.json"
        if ev_p.exists() and json.loads(ev_p.read_text()).get("cfg_hash") == h:
            print(f"[{scene}/{key}] cached")
            summary[key] = json.loads(ev_p.read_text())
            continue
        t0 = time.time()
        m = run_phase3(ws, out, cfg)
        ev = evaluate_phase3(ws, out, cache=cache, n_gt=sc.get("n_gt", 400_000))
        meta = json.loads((out / "meta.json").read_text())
        ev |= {
            "cfg_hash": h,
            "run": name,
            "tag": tag,
            "overrides": overrides or {},
            "wall_time_s": meta["wall_time_s"],
            "peak_ram_mb": meta["peak_ram_mb"],
            "timings": m["timings"],
            "n_points": m["n_points"],
            "frames_ok": m["frames_ok"],
            "frames_rejected": m["frames_rejected"],
            "valid_fraction": m["valid_fraction"],
            "scale_cv_raw": m.get("scale_cv_raw"),
            "scale_cv_smoothed": m.get("scale_cv_smoothed"),
        }
        ev_p.write_text(json.dumps(ev, indent=1))
        summary[key] = ev
        r = ev.get("recon") or {}
        nan = float("nan")
        msg = (
            f"F@2={r.get('fscore_2cm', nan):.3f} F@5={r.get('fscore_5cm', nan):.3f} "
            f"acc={r.get('accuracy', nan) * 100:.2f}cm comp={r.get('completion', nan) * 100:.2f}cm"
        )
        print(f"[{scene}/{key}] {time.time() - t0:.0f}s {msg} ram={meta['peak_ram_mb']:.0f}MB")
        summary_p.write_text(json.dumps(summary, indent=1))
    return summary


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--scene", required=True)
    r = sub.add_parser("run")
    r.add_argument("--scene", required=True)
    r.add_argument("--runs", nargs="+", default=list(RUNS))
    r.add_argument("--override", default="{}", help="JSON dict of Phase3Config overrides")
    r.add_argument("--tag", default="")
    a = ap.parse_args()
    if a.cmd == "prepare":
        print(prepare(a.scene))
    else:
        run(a.scene, a.runs, json.loads(a.override), a.tag)


if __name__ == "__main__":
    main()
