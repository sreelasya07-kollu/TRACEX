"""Phase 4 (HANDOFF P4): TSDF fusion of ray-cast depth vs the exact synthetic GT mesh.
Target: Chamfer-L1 <= 1 cm (GT culled to what the cameras saw, eval/recon_metrics.py)."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

from eval.recon_metrics import cull_to_cameras, recon_metrics, sample_mesh
from eval.synthetic_room import camera_model, cameras, fused_cloud, room_mesh

RESULTS = Path(__file__).resolve().parents[1] / "runs" / "test_results"


@pytest.mark.light
def test_synthetic_fusion_chamfer():
    pcd = fused_cloud(seed=0, noise_m=0.003)
    mesh = room_mesh()
    gt = sample_mesh(mesh, 200_000, seed=0)
    K, W, H = camera_model()
    cams = [(T, K, W, H) for T in cameras(0)]
    keep = cull_to_cameras(np.asarray(gt.points), cams, gt_mesh=mesh, max_depth=10.0)
    pred = np.asarray(pcd.points)
    m = recon_metrics(pred, np.asarray(gt.points)[keep], thresholds=(0.01, 0.02, 0.05))
    m |= {"gt_seen_fraction": float(keep.mean()), "noise_m": 0.003, "voxel_m": 0.02}
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "phase4_synthetic.json").write_text(json.dumps(m, indent=1))
    assert m["chamfer_l1"] <= 0.01, m
