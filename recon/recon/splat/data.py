"""Splat dataset from a SpatialMap: images, intrinsics, world->camera matrices, hold-out split,
and the initial Gaussian cloud (fused depth cloud, falling back to COLMAP sparse points)."""

from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np
import open3d as o3d
from scipy.spatial import cKDTree

from formats.spatial_map import SpatialMap

SH_C0 = 0.28209479177387814


@dataclass
class SplatView:
    frame_id: str
    image: np.ndarray  # float32 HxWx3 in [0, 1]
    K: np.ndarray  # 3x3
    viewmat: np.ndarray  # 4x4 world -> camera (OpenCV camera, as gsplat expects)


@dataclass
class SplatDataset:
    train: list[SplatView]
    heldout: list[SplatView]
    init_xyz: np.ndarray
    init_rgb: np.ndarray  # [0, 1]
    scene_scale: float


def scale_K(K: np.ndarray, factor: float) -> np.ndarray:
    """Intrinsics for an image resized by `factor` (pixel-centre convention)."""
    K2 = K.copy()
    K2[0, 0] *= factor
    K2[1, 1] *= factor
    K2[0, 2] = (K[0, 2] + 0.5) * factor - 0.5
    K2[1, 2] = (K[1, 2] + 0.5) * factor - 0.5
    return K2


def split_heldout(ids: list[str], every: int = 8) -> tuple[list[str], list[str]]:
    ids = sorted(ids)
    held = [i for k, i in enumerate(ids) if k % every == every - 1]
    return [i for i in ids if i not in held], held


def init_gaussians(
    xyz: np.ndarray,
    rgb: np.ndarray,
    sh_degree: int = 3,
    init_opacity: float = 0.1,
    init_scale: float = 1.0,
    seed: int = 0,
) -> dict[str, np.ndarray]:
    """Parameters as in the 3DGS reference: log-scales from the mean distance to the 3 nearest
    neighbours, identity-ish random quats, logit opacity, SH degree-0 colour from RGB."""
    n = len(xyz)
    d, _ = cKDTree(xyz).query(xyz, k=4)
    dist = np.sqrt((d[:, 1:] ** 2).mean(1)).clip(1e-4, None)
    rng = np.random.default_rng(seed)
    q = rng.normal(size=(n, 4))
    q /= np.linalg.norm(q, axis=1, keepdims=True)
    k_rest = (sh_degree + 1) ** 2 - 1
    return {
        "means": xyz.astype(np.float32),
        "scales": np.log(dist * init_scale)[:, None].repeat(3, 1).astype(np.float32),
        "quats": q.astype(np.float32),
        "opacities": np.full(n, np.log(init_opacity / (1 - init_opacity)), np.float32),
        "sh0": ((rgb - 0.5) / SH_C0).astype(np.float32).reshape(n, 1, 3),
        "shN": np.zeros((n, k_rest, 3), np.float32),
    }


def _init_cloud(sm: SpatialMap, max_points: int) -> tuple[np.ndarray, np.ndarray]:
    pcd = o3d.io.read_point_cloud(str(sm.fused_cloud))
    if len(pcd.points) < 1000:
        raise ValueError("fused cloud too small to initialise a splat scene")
    if len(pcd.points) > max_points:
        pcd = pcd.voxel_down_sample(
            float(np.cbrt(np.prod(np.ptp(np.asarray(pcd.points), 0)) / max_points))
        )
    rgb = np.asarray(pcd.colors) if pcd.has_colors() else np.full((len(pcd.points), 3), 0.5)
    return np.asarray(pcd.points), rgb


def load_dataset(
    sm: SpatialMap, image_scale: float = 0.5, heldout_every: int = 8, max_init_points: int = 200_000
) -> SplatDataset:
    cams = sm.cameras
    K0 = cams.intrinsics.K()
    views = {}
    for fid, T in cams.frames.items():
        img = cv2.imread(str(sm.images_dir / f"{fid}.jpg"))
        if img is None:
            raise FileNotFoundError(sm.images_dir / f"{fid}.jpg")
        if image_scale != 1.0:
            img = cv2.resize(
                img,
                (round(cams.width * image_scale), round(cams.height * image_scale)),
                interpolation=cv2.INTER_AREA,
            )
        views[fid] = SplatView(
            fid,
            (cv2.cvtColor(img, cv2.COLOR_BGR2RGB) / 255.0).astype(np.float32),
            scale_K(K0, image_scale),
            np.linalg.inv(np.asarray(T)),
        )
    train_ids, held_ids = split_heldout(list(views), heldout_every)
    xyz, rgb = _init_cloud(sm, max_init_points)
    centres = np.array([np.linalg.inv(v.viewmat)[:3, 3] for v in views.values()])
    scene_scale = float(np.linalg.norm(centres - centres.mean(0), axis=1).max())
    return SplatDataset(
        [views[i] for i in train_ids], [views[i] for i in held_ids], xyz, rgb, scene_scale
    )


__all__ = ["load_dataset", "split_heldout", "init_gaussians", "scale_K", "SH_C0"]
