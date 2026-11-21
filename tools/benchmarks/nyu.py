"""Step A: reproduce the published Depth-Anything-V2 NYUv2 number with our metric code.

Published reference (only comparable one): DAv2 paper (arXiv 2406.09414) Table 2,
zero-shot *relative* depth, ViT-S, NYU-D: AbsRel 0.053, delta1 0.973.
Protocol (affine-invariant, MiDaS/Depth-Anything zero-shot evaluation):
  - official labelled test split (654 images), GT = 'depths' field (metres)
  - Eigen crop rows 45:471, cols 41:601; valid GT in (1e-3, 10] m
  - per image, least-squares scale+shift of predicted disparity to GT disparity (1/gt)
    on valid pixels; depth = 1/max(aligned disparity, 1/10)
The metric model (Hypersim) is also evaluated zero-shot (raw and per-image median scale);
the paper publishes NO zero-shot NYU number for it.
  python -m benchmarks.nyu --model relative|metric [--n 654]
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np

from eval.depth_metrics import aggregate, depth_metrics
from spatial.depth.sources import MonoDepth

ROOT = Path(__file__).resolve().parents[1]
NYU = ROOT / "data" / "nyu" / "test"
OUT = ROOT / "runs" / "test_results"
PUBLISHED = {
    "relative": {
        "abs_rel": 0.053,
        "d1": 0.973,
        "source": "DAv2 arXiv:2406.09414 Table 2 (zero-shot relative, ViT-S, NYU-D)",
    }
}


def eigen_mask(shape):
    m = np.zeros(shape, bool)
    m[45:471, 41:601] = True
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", choices=["relative", "metric"], default="relative")
    ap.add_argument("--n", type=int, default=654)
    ap.add_argument("--device", default="cpu")
    a = ap.parse_args()
    idx = json.loads((NYU / "index.json").read_text())["mat_index_1based"][: a.n]
    mono = MonoDepth("small", a.model, a.device, input_size=518)
    paths = [NYU / "rgb" / f"{i:04d}.png" for i in idx]
    t0 = time.time()
    per, per_raw = [], []
    # Stream in chunks so memory stays bounded (an all-at-once run died at ~420/654 locally
    # with ~3.5 GB RAM available); cached predictions are reused on resume.
    chunk = 16
    for c in range(0, len(idx), chunk):
        for i, p in zip(idx[c : c + chunk], mono.predict(paths[c : c + chunk]), strict=True):
            _score(a.model, i, p, per, per_raw)
    infer_s = time.time() - t0
    _finish(a, mono, idx, per, per_raw, infer_s)


def _score(model, i, p, per, per_raw):
    gt = np.load(NYU / "depth" / f"{i:04d}.npy")
    v = eigen_mask(gt.shape) & (gt > 1e-3) & (gt <= 10)
    if model == "relative":
        A = np.c_[p[v], np.ones(v.sum())]
        s, t = np.linalg.lstsq(A, 1.0 / gt[v], rcond=None)[0]
        disp = np.maximum(s * p + t, 1 / 10.0)
        per.append(depth_metrics(1.0 / disp, gt, v, 1e-3, 10))
    else:
        per_raw.append(depth_metrics(p, gt, v, 1e-3, 10))
        per.append(depth_metrics(p * np.median(gt[v]) / np.median(p[v]), gt, v, 1e-3, 10))


def _finish(a, mono, idx, per, per_raw, infer_s):
    res = {
        "model": mono.info(),
        "n_images": len(idx),
        "protocol": __doc__.split("Protocol")[1],
        "wall_s_total": infer_s,
        "ms_per_image_uncached": float(np.mean(mono.timings_ms)) if mono.timings_ms else None,
        "n_inferred_this_run": len(mono.timings_ms) * mono.batch,
    }
    if a.model == "relative":
        res["ours"] = aggregate(per)
        pub = PUBLISHED["relative"]
        res["published"] = pub
        res["rel_diff"] = {k: (res["ours"][k] - pub[k]) / pub[k] for k in ("abs_rel", "d1")}
    else:
        res["ours_raw_metric"] = aggregate(per_raw)
        res["ours_median_scaled"] = aggregate(per)
        res["published"] = "none for zero-shot Hypersim metric model on NYU"
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f"phase3_nyu_{a.model}.json").write_text(json.dumps(res, indent=1))
    print(json.dumps({k: v for k, v in res.items() if k not in ("protocol", "model")}, indent=1))


if __name__ == "__main__":
    main()
