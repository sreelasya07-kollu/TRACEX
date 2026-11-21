"""Evaluate a Phase 3 output against ground truth (eval only; reads workspace/gt)."""

from __future__ import annotations

import json
from pathlib import Path

import cv2
import numpy as np
import open3d as o3d

from eval.depth_metrics import aggregate, depth_metrics
from eval.gt import load_mesh, resolve_gt_path
from eval.recon_metrics import cull_to_cameras, recon_metrics, sample_mesh
from spatial.geometry import umeyama
from spatial.phase3 import load_poses as load_est_poses


def _G(ws: Path) -> np.ndarray:
    """our world -> GT/prior world (inverse of the Phase 2 gravity rotation)."""
    a = json.loads((ws / "alignment.json").read_text()) if (ws / "alignment.json").exists() else {}
    G = np.eye(4)
    G[:3, :3] = np.asarray(a.get("R_world_prior", np.eye(3))).T
    return G


def evaluate_phase3(
    ws: Path,
    out: Path,
    gt_mesh: Path | None = None,
    n_gt: int = 400_000,
    max_depth: float = 8.0,
    seed: int = 0,
    cache: dict | None = None,
) -> dict:
    meta = json.loads((out / "meta.json").read_text())
    ids = meta["metrics"]["frame_ids"]
    W, H = meta["metrics"]["width"], meta["metrics"]["height"]
    D = meta["params"]["downscale"]
    pri = json.loads((ws / "priors.json").read_text())
    intr = pri["intrinsics"]
    K = np.array(
        [
            [intr["fx"] / D, 0, (intr["cx"] + 0.5) / D - 0.5],
            [0, intr["fy"] / D, (intr["cy"] + 0.5) / D - 0.5],
            [0, 0, 1],
        ]
    )
    gt_poses = json.loads((ws / "gt" / "poses_gt.json").read_text())
    res: dict = {"frame_ids": ids}

    # ---- 3D metrics ------------------------------------------------------------------------
    if gt_mesh is None:
        ref = json.loads((ws / "gt" / "gt_ref.json").read_text())
        gt_mesh = resolve_gt_path(ref["geometry"]) if ref.get("geometry") else None
    if gt_mesh is not None and gt_mesh.suffix == ".ply":
        key = (str(gt_mesh), tuple(ids), W, H, n_gt, seed)
        if cache is not None and key in cache:
            gtp, gtn = cache[key]
        else:
            mesh = load_mesh(gt_mesh)
            pc = sample_mesh(mesh, n_gt, seed)
            cams = [(np.asarray(gt_poses[i]), K, W, H) for i in ids]
            keep = cull_to_cameras(np.asarray(pc.points), cams, gt_mesh=mesh, max_depth=max_depth)
            gtp, gtn = np.asarray(pc.points)[keep], np.asarray(pc.normals)[keep]
            if cache is not None:
                cache[key] = (gtp, gtn)
        pcd = o3d.io.read_point_cloud(str(out / "fusion" / "cloud.ply"))
        pcd.transform(_G(ws))
        pp_abs = np.asarray(pcd.points)
        pn_abs = np.asarray(pcd.normals) if pcd.has_normals() else None
        cams = [(np.asarray(gt_poses[i]), K, W, H) for i in ids]
        # Trajectory-aligned protocol: SE(3) (no scale) from the *estimated camera centres* to
        # the GT centres of the same frames; never uses GT geometry. Removes the global offset
        # inherited from drifting VIO priors while keeping metric scale under test.
        src = meta["params"]["pose_source"]
        used = out / "poses_used.json"
        est = (
            {k: np.asarray(v) for k, v in json.loads(used.read_text()).items()}
            if used.exists()
            else load_est_poses(ws, src)
        )
        P = np.array([(_G(ws) @ est[i])[:3, 3] for i in ids])
        Q = np.array([np.asarray(gt_poses[i])[:3, 3] for i in ids])
        _, R, t = umeyama(P, Q, with_scale=False)
        res["traj_align"] = {
            "R": R.tolist(),
            "t": t.tolist(),
            "rmse_m": float(np.sqrt(((P @ R.T + t - Q) ** 2).sum(1).mean())),
        }
        variants = {
            "recon": (pp_abs @ R.T + t, None if pn_abs is None else pn_abs @ R.T),
            "recon_absolute": (pp_abs, pn_abs),
        }
        res["n_gt_culled"] = int(len(gtp))
        for name, (pp, pn) in variants.items():
            pkeep = (
                cull_to_cameras(pp, cams, gt_mesh=None, max_depth=max_depth)
                if len(pp)
                else np.zeros(0, bool)
            )
            res[f"{name}_pred_frustum_kept"] = float(np.mean(pkeep)) if len(pp) else 0.0
            res[name] = (
                recon_metrics(pp[pkeep], gtp, pn[pkeep] if pn is not None else None, gtn)
                if len(pp) and pkeep.any()
                else None
            )

    # ---- depth metrics vs sensor / GT depth (on frames with depth) ----------------------------
    per_raw, per_final = [], []
    for i in ids:
        f = next(x for x in pri["frames"] if x["id"] == i)
        if not f.get("depth"):
            continue
        gt = (
            cv2.resize(
                cv2.imread(str(ws / f["depth"]), cv2.IMREAD_UNCHANGED),
                (W, H),
                interpolation=cv2.INTER_NEAREST,
            ).astype(np.float32)
            / 1000.0
        )
        d = cv2.imread(str(out / "depth_aligned" / f"{i}.png"), cv2.IMREAD_UNCHANGED) / 1000.0
        v = cv2.imread(str(out / "depth_valid" / f"{i}.png"), cv2.IMREAD_UNCHANGED) > 0
        if not (d > 0).any():
            continue
        per_raw.append(depth_metrics(d, gt, mask=d > 0, min_depth=0.1, max_depth=max_depth))
        if v.any():
            per_final.append(depth_metrics(d, gt, mask=v, min_depth=0.1, max_depth=max_depth))
    if per_raw:
        res["depth_vs_sensor_all"] = aggregate(per_raw)
    if per_final:
        res["depth_vs_sensor_valid"] = aggregate(per_final)
    return res
