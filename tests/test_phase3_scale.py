"""Phase 3 scale recovery (HANDOFF P3): a metric sparse model is deliberately scaled by 0.37;
depth-based scale estimation must recover 1/0.37 within 2 %. Real sensor depth (TUM Kinect,
Replica rendered depth). Measured values -> runs/test_results/phase3_scale_recovery.json."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pycolmap
import pytest

from formats.models import Priors
from spatial.sfm.refine import depth_scale_factor

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "runs" / "test_results"
WORKSPACES = {
    "tum_xyz": ROOT / "runs/dev_p2_tum_xyz/workspace",
    "replica_room0": ROOT / "runs/bench_phase3/room0/_workspace/workspace",
}
FACTOR = 0.37


@pytest.mark.heavy
@pytest.mark.parametrize("name", sorted(WORKSPACES))
def test_depth_scale_recovers_deliberate_scaling(name):
    ws = WORKSPACES[name]
    if not (ws / "sparse" / "0" / "images.bin").exists():
        pytest.skip(f"missing workspace {ws} (run Phase 1+2)")
    pri = Priors.model_validate_json((ws / "priors.json").read_text())
    model = pycolmap.Reconstruction(ws / "sparse" / "0")
    base = depth_scale_factor(model, ws, pri)  # already metric: should be ~1
    model.transform(pycolmap.Sim3d(FACTOR, pycolmap.Rotation3d(np.eye(3)), np.zeros((3, 1))))
    shrunk = depth_scale_factor(model, ws, pri)
    err = abs(shrunk["scale"] * FACTOR - 1.0)
    RESULTS.mkdir(parents=True, exist_ok=True)
    f = RESULTS / "phase3_scale_recovery.json"
    all_ = json.loads(f.read_text()) if f.exists() else {}
    all_[name] = {
        "factor_applied": FACTOR,
        "recovered_scale": shrunk["scale"],
        "expected": 1 / FACTOR,
        "scale_error": err,
        "metric_model_scale": base["scale"],
        "n_images": shrunk["n_images"],
    }
    f.write_text(json.dumps(all_, indent=1))
    assert shrunk["status"] == "ok" and err <= 0.02
