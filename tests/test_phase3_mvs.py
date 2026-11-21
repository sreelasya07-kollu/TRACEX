"""Phase 3 MVS depth vs Kinect depth on TUM fr1/xyz (HANDOFF P3: AbsRel <= 0.10, d<1.25 >= 0.90).

OpenMVS depth is only compared where its own confidence >= CONF_MIN (the production default,
Phase3Config.mvs_conf_min). The unfiltered numbers are saved next to it and are NOT within the
target; both are in runs/test_results/phase3_mvs_tum.json. Needs docker image room-openmvs:2.4.0
(CPU; ~3 min for 60 frames at 640x480 the first time, cached afterwards)."""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import cv2
import numpy as np
import pytest

from eval.depth_metrics import aggregate, depth_metrics
from spatial.depth.mvs import MVSParams, load_mvs_depths, run_openmvs_depth
from spatial.phase3 import Phase3Config

ROOT = Path(__file__).resolve().parents[1]
WS = ROOT / "runs/dev_p2_tum_xyz/workspace"
CACHE = ROOT / "runs/dev_p3_mvs/tum_xyz"
CONF_MIN = Phase3Config().mvs_conf_min


@pytest.mark.heavy
def test_mvs_depth_vs_kinect_tum():
    if not (WS / "sparse/0/images.bin").exists():
        pytest.skip("needs runs/dev_p2_tum_xyz (Phase 1+2)")
    if not list(CACHE.glob("depth*.dmap")):
        if shutil.which("docker") is None:
            pytest.skip("docker not available")
        run_openmvs_depth(WS, CACHE, MVSParams(resolution_level=1))
    W, H = 320, 240
    mvs = load_mvs_depths(CACHE, (W, H))
    out = {}
    for name, thr in [("unfiltered", None), (f"conf>={CONF_MIN}", CONF_MIN)]:
        per = []
        for i, (d, c) in mvs.items():
            g = cv2.resize(
                cv2.imread(str(WS / f"depth/{i}.png"), cv2.IMREAD_UNCHANGED).astype(np.float32)
                / 1000,
                (W, H),
                interpolation=cv2.INTER_NEAREST,
            )
            m = d > 0
            if thr is not None and c is not None:
                m &= c >= thr
            per.append(depth_metrics(d, g, m, 0.1, 8.0) | {"coverage": float(m.mean())})
        agg = aggregate(per)
        agg["coverage"] = float(np.mean([p["coverage"] for p in per]))
        out[name] = {k: float(v) for k, v in agg.items()}
    out["n_frames"] = len(mvs)
    out["conf_min"] = CONF_MIN
    p = ROOT / "runs/test_results/phase3_mvs_tum.json"
    p.write_text(json.dumps(out, indent=1))
    sel = out[f"conf>={CONF_MIN}"]
    assert len(mvs) >= 55
    assert sel["abs_rel"] <= 0.10 and sel["d1"] >= 0.90
