"""Phase 1 keyframe selection (HANDOFF P1: keyframes cover the trajectory; no gap above
0.3 m or 15 deg). light: injected blur is avoided. heavy: TUM fr1/desk (real motion blur),
gaps measured on GT poses."""

from __future__ import annotations

import json
from pathlib import Path

import cv2
import numpy as np
import pytest

from spatial.io.keyframes import KeyframeParams, max_gap, select_keyframes, sharpness

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "runs" / "test_results"


def _line_poses(n, step):
    out = []
    for i in range(n):
        T = np.eye(4)
        T[0, 3] = i * step
        out.append(T)
    return out


@pytest.mark.light
def test_blurred_frames_are_avoided_and_gaps_bounded(tmp_path):
    rng = np.random.default_rng(0)
    base = (rng.uniform(0, 255, (240, 320)) > 128).astype(np.uint8) * 255
    base = cv2.GaussianBlur(base, (3, 3), 0)
    n = 60
    blurred = set(range(5, 60, 4))  # every 4th frame is motion-blurred
    sharp = []
    for i in range(n):
        img = cv2.GaussianBlur(base, (0, 0), 6) if i in blurred else base
        cv2.imwrite(str(tmp_path / f"{i}.png"), img)
        sharp.append(sharpness(tmp_path / f"{i}.png"))
    sharp = np.array(sharp)
    assert sharp[list(blurred)].max() < 0.2 * sharp[[i for i in range(n) if i not in blurred]].min()
    poses = _line_poses(n, 0.02)  # 2 cm per frame
    sel = select_keyframes(sharp, poses, KeyframeParams())
    assert not set(sel) & blurred
    dt, dr = max_gap([poses[i] for i in sel])
    assert dt <= 0.3 and dr <= 15 and 5 <= len(sel) < n


@pytest.mark.light
def test_without_poses_only_blur_filter():
    s = np.array([100.0, 100, 5, 100, 100])
    assert select_keyframes(s, [None] * 5) == [0, 1, 3, 4]


@pytest.mark.heavy
def test_tum_desk_keyframes_cover_trajectory(tmp_path):
    from eval.trajectory import ate  # noqa: F401
    from spatial.io.ingest import ingest

    src = ROOT / "data/tum/rgbd_dataset_freiburg1_desk"
    if not src.exists():
        pytest.skip("dataset missing (data/fetch.sh)")
    ws = ingest("tum", src, tmp_path, stride=1, downscale=2, prior="gt", keyframes=KeyframeParams())
    gt = json.loads((ws / "gt/poses_gt.json").read_text())
    ids = sorted(gt)
    sel_poses = [np.asarray(gt[i]) for i in ids]
    dt, dr = max_gap(sel_poses)
    sh_sel = np.mean([sharpness(ws / "images" / f"{i}.jpg") for i in ids])
    # baseline: stride-k selection with the same number of frames
    from spatial.io.adapters import load_tum

    ds = load_tum(src)
    k = max(1, len(ds.frames) // len(ids))
    base = ds.frames[::k]
    sh_base = np.mean([sharpness(f.rgb) for f in base])
    bt, br = max_gap([f.T_world_camera_gt for f in base])
    out = {
        "n_total": len(ds.frames),
        "n_keyframes": len(ids),
        "max_gap_m": dt,
        "max_gap_deg": dr,
        "mean_sharpness_keyframes": float(sh_sel),
        "mean_sharpness_uniform_same_count": float(sh_base),
        "uniform_max_gap_m": bt,
        "uniform_max_gap_deg": br,
    }
    (RESULTS / "phase1_keyframes_tum_desk.json").write_text(json.dumps(out, indent=1))
    assert dt <= 0.3 and dr <= 15.0
    assert sh_sel > sh_base  # selection actually prefers sharp frames
