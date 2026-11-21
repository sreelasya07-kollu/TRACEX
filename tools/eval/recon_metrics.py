"""3D reconstruction metrics with GT culling to the observed region.

Definitions (both clouds in metres, same world frame):
  accuracy      mean distance pred -> nearest GT point
  completion    mean distance GT -> nearest pred point (on the *culled* GT)
  chamfer_l1    (accuracy + completion) / 2
  precision@t   fraction of pred points within t of GT
  recall@t      fraction of culled-GT points within t of pred
  fscore@t      2PR / (P + R)
  normal_consistency  mean |n_pred · n_gt| over nearest-neighbour pairs, averaged both ways

GT culling (documented procedure, docs/INTERFACES.md / phase_3 report):
  A GT sample point is kept iff, for at least one input camera, (1) it projects inside the
  image, (2) its camera depth z is in (near, max_depth], and (3) it is not occluded:
  z <= D_gt(u, v) + occ_tol, where D_gt is the GT mesh depth ray-cast from that camera.
  Prediction points are frustum-culled (1+2 only) with the same cameras.
  Without culling, completion penalises surfaces no camera ever saw.
"""

from __future__ import annotations

import numpy as np
import open3d as o3d
from scipy.spatial import cKDTree


def sample_mesh(mesh: o3d.geometry.TriangleMesh, n: int, seed: int = 0) -> o3d.geometry.PointCloud:
    o3d.utility.random.seed(seed)
    if not mesh.has_triangle_normals():
        mesh.compute_triangle_normals()
    return mesh.sample_points_uniformly(n, use_triangle_normal=True)


def _project(points: np.ndarray, T_cw: np.ndarray, K: np.ndarray):
    Xc = points @ T_cw[:3, :3].T + T_cw[:3, 3]
    z = Xc[:, 2]
    with np.errstate(divide="ignore", invalid="ignore"):
        uv = (Xc[:, :2] / z[:, None]) * [K[0, 0], K[1, 1]] + [K[0, 2], K[1, 2]]
    return uv, z


def cull_to_cameras(
    points: np.ndarray,
    cameras: list[tuple[np.ndarray, np.ndarray, int, int]],
    gt_mesh: o3d.geometry.TriangleMesh | None = None,
    near: float = 0.05,
    max_depth: float = 8.0,
    occ_tol: float = 0.03,
) -> np.ndarray:
    """Boolean mask of `points` seen by >= 1 camera. cameras: (T_world_camera, K, W, H).
    If gt_mesh is given, an occlusion test against its ray-cast depth is applied."""
    seen = np.zeros(len(points), bool)
    scene = None
    if gt_mesh is not None:
        scene = o3d.t.geometry.RaycastingScene()
        scene.add_triangles(o3d.t.geometry.TriangleMesh.from_legacy(gt_mesh))
    for T_wc, K, W, H in cameras:
        T_cw = np.linalg.inv(T_wc)
        todo = ~seen
        uv, z = _project(points[todo], T_cw, K)
        ok = (
            (z > near)
            & (z <= max_depth)
            & (uv[:, 0] >= -0.5)
            & (uv[:, 0] < W - 0.5)
            & (uv[:, 1] >= -0.5)
            & (uv[:, 1] < H - 0.5)
        )
        if scene is not None and ok.any():
            rays = o3d.t.geometry.RaycastingScene.create_rays_pinhole(
                o3d.core.Tensor(K), o3d.core.Tensor(T_cw), W, H
            )
            t = scene.cast_rays(rays)["t_hit"].numpy()
            dz = rays.numpy()[..., 3:] @ T_cw[:3, :3].T
            Dgt = t * dz[..., 2]
            ui = np.clip(np.round(uv[ok, 0]).astype(int), 0, W - 1)
            vi = np.clip(np.round(uv[ok, 1]).astype(int), 0, H - 1)
            vis = z[ok] <= np.where(np.isfinite(Dgt[vi, ui]), Dgt[vi, ui], np.inf) + occ_tol
            idx = np.flatnonzero(ok)
            ok[idx[~vis]] = False
        sub = np.flatnonzero(todo)
        seen[sub[ok]] = True
    return seen


def recon_metrics(
    pred: np.ndarray,
    gt: np.ndarray,
    pred_normals: np.ndarray | None = None,
    gt_normals: np.ndarray | None = None,
    thresholds=(0.02, 0.05),
) -> dict[str, float]:
    if len(pred) == 0 or len(gt) == 0:
        raise ValueError("empty point set")
    d_p2g, i_p2g = cKDTree(gt).query(pred, workers=-1)
    d_g2p, i_g2p = cKDTree(pred).query(gt, workers=-1)
    out = {
        "n_pred": len(pred),
        "n_gt": len(gt),
        "accuracy": float(d_p2g.mean()),
        "completion": float(d_g2p.mean()),
        "accuracy_median": float(np.median(d_p2g)),
        "completion_median": float(np.median(d_g2p)),
    }
    out["chamfer_l1"] = (out["accuracy"] + out["completion"]) / 2
    for t in thresholds:
        P = float((d_p2g < t).mean())
        R = float((d_g2p < t).mean())
        tag = f"{round(t * 100)}cm"
        out |= {
            f"precision_{tag}": P,
            f"recall_{tag}": R,
            f"fscore_{tag}": 0.0 if P + R == 0 else 2 * P * R / (P + R),
        }
    if pred_normals is not None and gt_normals is not None:
        a = np.abs(np.sum(pred_normals * gt_normals[i_p2g], 1)).mean()
        b = np.abs(np.sum(gt_normals * pred_normals[i_g2p], 1)).mean()
        out["normal_consistency"] = float((a + b) / 2)
    return out
