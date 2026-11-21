"""T-027 (issue #3): COLMAP's internal resize vs INTER_AREA pre-resize in ingest.

  uv run python scripts/heavy/preresize_measure.py --scene eth3d_office
  uv run python scripts/heavy/preresize_measure.py --scene replica_room0

Per scene, two configs, `--runs` (default 5) seeded multithreaded `refine` runs each on ONE
ingested workspace per config (the stage records outputs relative to the run dir, so the
workspace must sit inside it). Writes runs/test_results/preresize_<scene>.json with every run
(registered, mean reproj, ATE Sim(3) median/rmse/rotation, wall time) and per-config summaries
(min registered, median reproj, median ATE). Report only: nothing here changes a default.

  config a  "colmap_internal":  workspace at source/downscale resolution, COLMAP resizes to
                                refine max_image_size
  config b  "inter_area_preresize": ingest --max-side == the same long side, refine unchanged
"""

from __future__ import annotations

import argparse
import json
import tempfile
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
RESULTS = REPO / "runs" / "test_results"

# scene -> (dataset, root, common ingest kwargs, refine kwargs, config a ingest, config b ingest)
SCENES = {
    # ETH3D office dslr (6200x4130-class): /2 -> ~3100 px, COLMAP then resizes to 1600 (ratio ~1.9).
    "eth3d_office": dict(
        dataset="eth3d",
        root=REPO / "data" / "eth3d" / "office",
        ingest=dict(prior="none"),
        refine=dict(max_image_size=1600, matcher="exhaustive"),
        a=dict(downscale=2),
        b=dict(max_side=1600),
    ),
    # Replica room0 1200x680 -> default max_image_size 1024 (ratio 1.17, non-integer).
    "replica_room0": dict(
        dataset="replica",
        root=REPO / "data" / "replica" / "room0",
        ingest=dict(prior="none", max_frames=80, stride=5),
        refine=dict(max_image_size=1024, matcher="exhaustive"),
        a=dict(),
        b=dict(max_side=1024),
    ),
}


def _med(xs):
    xs = [x for x in xs if x is not None]
    return float(np.median(xs)) if xs else None


def _summarise(runs):
    return {
        "registered_min": min(r["registered"] for r in runs),
        "registered_median": _med([r["registered"] for r in runs]),
        "full_runs": sum(r["registered"] == r["n_frames"] for r in runs),
        "reproj_median_px": _med([r["mean_reproj_px"] for r in runs]),
        "ate_sim3_median_m": _med([r["ate_sim3_median_m"] for r in runs]),
        "ate_sim3_rmse_median_m": _med([r["ate_sim3_rmse_m"] for r in runs]),
        "rot_mean_median_deg": _med([r["rot_mean_deg"] for r in runs]),
        "wall_s_median": _med([r["wall_s"] for r in runs]),
    }


def measure(scene: str, n_runs: int, tmp: Path) -> dict:
    from eval.trajectory import ate
    from spatial.io.ingest import ingest
    from spatial.sfm.refine import poses_in_gt_frame, refine

    sc = SCENES[scene]
    if not sc["root"].exists():
        raise SystemExit(f"dataset missing: {sc['root']}")
    out = {"scene": scene, "n_runs": n_runs, "refine": sc["refine"], "configs": {}}
    for name, label in (("a", "colmap_internal"), ("b", "inter_area_preresize")):
        run = tmp / name
        run.mkdir(parents=True)
        ikw = sc["ingest"] | sc[name]
        ws = ingest(sc["dataset"], sc["root"], run, **ikw)
        pri = json.loads((ws / "priors.json").read_text())
        gt = {
            k: np.asarray(v) for k, v in json.loads((ws / "gt/poses_gt.json").read_text()).items()
        }
        runs = []
        for seed in range(n_runs):
            t0 = time.time()
            r = refine(ws, run, seed=seed, num_threads=-1, **sc["refine"])
            wall = time.time() - t0
            row = {
                "seed": seed,
                "status": r.get("status", "ok"),
                "registered": r.get("registered", 0),
                "n_frames": r.get("n_frames"),
                "mean_reproj_px": r.get("mean_reproj_px"),
                "model_sizes": r.get("model_sizes"),
                "wall_s": wall,
                "ate_sim3_median_m": None,
                "ate_sim3_rmse_m": None,
                "rot_mean_deg": None,
            }
            if row["registered"] >= 3:
                a = ate(poses_in_gt_frame(ws), gt, "sim3")
                row |= {
                    "ate_sim3_median_m": a["median_m"],
                    "ate_sim3_rmse_m": a["rmse_m"],
                    "rot_mean_deg": a["rot_mean_deg"],
                }
            runs.append(row)
            print(label, seed, row, flush=True)
        out["configs"][label] = {
            "ingest": {k: v for k, v in ikw.items() if k != "prior"},
            "workspace_size": [pri["width"], pri["height"]],
            "runs": runs,
            "summary": _summarise(runs),
        }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", choices=sorted(SCENES), required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--tmp", type=Path, default=None, help="work dir (default: system temp)")
    args = ap.parse_args()
    if args.tmp is not None:
        args.tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.tmp) as t:
        res = measure(args.scene, args.runs, Path(t))
    RESULTS.mkdir(parents=True, exist_ok=True)
    p = RESULTS / f"preresize_{args.scene}.json"
    p.write_text(json.dumps(res, indent=1))
    print(json.dumps({k: v["summary"] for k, v in res["configs"].items()}, indent=1))
    print("wrote", p)


if __name__ == "__main__":
    main()
