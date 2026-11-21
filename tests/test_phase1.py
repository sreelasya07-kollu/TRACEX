"""Phase 1 tests. light = synthetic/controlled; heavy = real data (writes measured values)."""

from __future__ import annotations

import json
from pathlib import Path

import cv2
import numpy as np
import open3d as o3d
import pytest

from eval.gt import load_mesh
from formats.models import Priors
from spatial.geometry import invert, umeyama
from spatial.io.adapters import load_eth3d, load_replica, load_tum
from spatial.io.ingest import NOISE_PRESETS, ingest, simulate_vio_prior

DATA = Path(__file__).resolve().parents[1] / "data"
RESULTS = Path(__file__).resolve().parents[1] / "runs" / "test_results"


def _save(name: str, values: dict):
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / f"{name}.json").write_text(json.dumps(values, indent=1))


def _need(p: Path):
    if not p.exists():
        pytest.skip(f"dataset missing: {p} (run data/fetch.sh)")


@pytest.mark.light
def test_vio_noise_is_yaw_only_and_drift_grows():
    """Simulated VIO prior keeps gravity (no roll/pitch drift) and drift grows with path."""
    T = []
    for k in range(200):  # straight 10 m walk along x, camera looking along +x
        M = np.eye(4)
        M[:3, :3] = np.array([[0, 0, 1], [-1, 0, 0], [0, -1, 0]])
        M[:3, 3] = [0.05 * k, 0, 1.5]
        T.append(M)
    g = np.array([0, 0, -1.0])
    noisy, _ = simulate_vio_prior(T, g, "vio_large", seed=1)
    tilt = []
    for a, b in zip(T, noisy, strict=True):
        dR = b[:3, :3] @ a[:3, :3].T
        tilt.append(np.degrees(np.arccos(np.clip((dR @ [0, 0, 1])[2], -1, 1))))
    jitter = NOISE_PRESETS["vio_large"]["jitter_rot_deg"]
    assert max(tilt) < 5 * jitter  # only the white jitter tilts the camera; drift is yaw-only
    err = np.array([np.linalg.norm(a[:3, 3] - b[:3, 3]) for a, b in zip(T, noisy, strict=True)])
    assert err[-50:].mean() > err[:50].mean()


@pytest.mark.light
def test_downscale_intrinsics_pixel_centre():
    """A 3D point projects to the same *physical* location after downscale (pixel-centre rule)."""
    K = np.array([[600.0, 0, 599.5], [0, 600.0, 339.5], [0, 0, 1]])
    X = np.array([0.3, -0.2, 2.0])
    u = K @ X / X[2]
    D = 4
    K2 = K.copy()
    K2[:2, :2] /= D
    K2[:2, 2] = (K[:2, 2] + 0.5) / D - 0.5
    u2 = K2 @ X / X[2]
    assert np.allclose((u2[:2] + 0.5) * D - 0.5, u[:2], atol=1e-9)


@pytest.mark.heavy
def test_eth3d_pose_convention_reprojection():
    """Reproject ETH3D's 3D points with our converted poses; must match COLMAP's (~1 px)."""
    root = DATA / "eth3d" / "office"
    _need(root)
    ds = load_eth3d(root)
    pts = {}
    for ln in (root / "dslr_calibration_undistorted" / "points3D.txt").read_text().splitlines():
        if ln and not ln.startswith("#"):
            s = ln.split()
            pts[int(s[0])] = np.array(list(map(float, s[1:4])))
    lines = [
        ln
        for ln in (root / "dslr_calibration_undistorted" / "images.txt").read_text().splitlines()
        if not ln.startswith("#")
    ]
    by_name = {f.rgb.name: f for f in ds.frames}
    errs = []
    for head, obs in zip(lines[0::2], lines[1::2], strict=False):
        f = by_name[Path(head.split()[9]).name]
        T_cw = invert(f.T_world_camera_gt)
        o = obs.split()
        for k in range(0, len(o), 3):
            pid = int(o[k + 2])
            if pid < 0:
                continue
            Xc = T_cw[:3, :3] @ pts[pid] + T_cw[:3, 3]
            uv = (ds.K @ Xc)[:2] / Xc[2]
            errs.append(np.linalg.norm(uv - [float(o[k]), float(o[k + 1])]))
    errs = np.array(errs)
    _save(
        "phase1_eth3d_reprojection",
        {
            "n_obs": len(errs),
            "median_px": float(np.median(errs)),
            "p95_px": float(np.percentile(errs, 95)),
        },
    )
    assert np.median(errs) < 2.0


@pytest.mark.heavy
def test_tum_ingest_association_and_gravity(tmp_path):
    root = DATA / "tum" / "rgbd_dataset_freiburg1_xyz"
    _need(root)
    ds = load_tum(root)
    note = next(n for n in ds.notes if n.startswith("gravity"))
    spread = float(note.split("angular spread ")[1].split(" deg")[0])
    ws = ingest("tum", root, tmp_path, max_frames=30, stride=10, downscale=2, prior="gt_noisy")
    pri = Priors.model_validate_json((ws / "priors.json").read_text())
    meta = json.loads((tmp_path / "phase1" / "meta.json").read_text())
    d = cv2.imread(str(ws / pri.frames[0].depth), cv2.IMREAD_UNCHANGED)
    valid = d[d > 0] / 1000.0
    _save(
        "phase1_tum",
        {
            "gravity_spread_deg": spread,
            "notes": ds.notes,
            "n_frames": len(pri.frames),
            "depth_median_m": float(np.median(valid)),
            "prior_pos_err_mean_m": meta["metrics"]["prior_pos_err_mean_m"],
            "peak_ram_mb": meta["peak_ram_mb"],
        },
    )
    assert "max dt" in ds.notes[0] and float(ds.notes[0].split("max dt ")[1].split()[0]) <= 20
    assert spread < 3.0  # accelerometer direction consistent in GT world
    assert d.dtype == np.uint16 and d.shape == (240, 320)
    assert 0.3 < np.median(valid) < 4.0  # desk scene at ~1 m
    assert pri.pose_source == "dataset_gt_noisy" and meta["status"] == "ok"


@pytest.mark.heavy
def test_replica_pose_convention_and_gravity(tmp_path):
    """Ray-cast the GT mesh with our poses+K and compare to Replica's rendered depth.
    Also check the floor normal is +Z (justifies gravity = -Z for Replica)."""
    root = DATA / "replica" / "room0"
    _need(root / "SUBSET.json")
    ds = load_replica(root)
    mesh = load_mesh(root.parent / "room0_mesh.ply")
    scene = o3d.t.geometry.RaycastingScene()
    scene.add_triangles(o3d.t.geometry.TriangleMesh.from_legacy(mesh))
    # Ray-cast at full resolution and compare the *same* pixels (every 4th) to avoid the
    # half-block offset of nearest-neighbour downsampling.
    W, H = ds.width, ds.height
    med = []
    for f in ds.frames[:: max(1, len(ds.frames) // 10)]:
        T_cw = invert(f.T_world_camera_gt)
        rays = o3d.t.geometry.RaycastingScene.create_rays_pinhole(
            o3d.core.Tensor(ds.K), o3d.core.Tensor(T_cw), W, H
        )
        t = scene.cast_rays(rays)["t_hit"].numpy()[::4, ::4]
        z = t * (rays.numpy()[::4, ::4, 3:] @ T_cw[:3, :3].T)[..., 2]
        gt = cv2.imread(str(f.depth), cv2.IMREAD_UNCHANGED)[::4, ::4] * ds.depth_scale_m
        ok = np.isfinite(z) & (gt > 0)
        med.append(float(np.median(np.abs(z[ok] - gt[ok]))))
        med.append(float(np.median(np.abs(z[ok] - gt[ok]))))
    o3d.utility.random.seed(0)
    pc = mesh.sample_points_uniformly(200000)
    pts = np.asarray(pc.points)
    low = pc.select_by_index(np.where(pts[:, 2] < np.percentile(pts[:, 2], 15))[0])
    (a, b, c, _), inl = low.segment_plane(0.02, 3, 2000)
    floor_tilt = float(np.degrees(np.arccos(abs(c))))
    _save(
        "phase1_replica",
        {
            "depth_vs_raycast_median_abs_m": med,
            "floor_tilt_deg": floor_tilt,
            "floor_inliers": len(inl),
        },
    )
    assert max(med) < 0.005
    assert floor_tilt < 1.0


@pytest.mark.light
def test_umeyama_recovers_sim3():
    rng = np.random.default_rng(0)
    X = rng.normal(size=(50, 3))
    from scipy.spatial.transform import Rotation

    R = Rotation.random(random_state=1).as_matrix()
    Y = 2.5 * X @ R.T + [1, -2, 3]
    s, R2, t = umeyama(X, Y)
    assert abs(s - 2.5) < 1e-9 and np.allclose(R2, R, atol=1e-9) and np.allclose(t, [1, -2, 3])
