"""CPU rendering of a textured mesh at a camera (no OpenGL / GPU): Open3D ray casting for
visibility + barycentric UV interpolation + texture lookup. Used for held-out-view evaluation
(PSNR/SSIM) of the textured mesh. Pinhole cameras, OpenCV convention, T_world_camera poses."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import open3d as o3d
import trimesh


@dataclass
class _Part:
    faces: np.ndarray  # (F, 3) int
    uv: np.ndarray | None  # (V, 2) per vertex
    tex: np.ndarray | None  # (h, w, 3) uint8 RGB
    gray: np.ndarray  # fallback colour for untextured parts


def _bilinear(tex: np.ndarray, x: np.ndarray, y: np.ndarray) -> np.ndarray:
    """Bilinear texture lookup at pixel-centre coordinates (x, y), border-replicated."""
    h, w = tex.shape[:2]
    x = np.clip(x, 0, w - 1)
    y = np.clip(y, 0, h - 1)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    t = tex.astype(np.float32)
    top = t[y0, x0] * (1 - fx) + t[y0, x1] * fx
    bot = t[y1, x0] * (1 - fx) + t[y1, x1] * fx
    return np.clip(top * (1 - fy) + bot * fy + 0.5, 0, 255).astype(np.uint8)


class TexturedMesh:
    def __init__(self, obj_or_glb: Path, v_up: bool = True):
        """`v_up=True`: standard OBJ/MTL convention (v = 0 at the bottom of the image).
        OpenMVS TextureMesh output needs `v_up=False` (measured: sampling it as v-up puts 95 % of
        face centroids on black texels, v-down 24 %; held-out PSNR confirms, see test_phase8)."""
        self.v_up = v_up
        sc = trimesh.load(str(obj_or_glb), process=False, force="scene")
        self.parts: list[_Part] = []
        self.scene = o3d.t.geometry.RaycastingScene()
        for g in sc.geometry.values():
            if not isinstance(g, trimesh.Trimesh) or len(g.faces) == 0:
                continue
            uv, tex = None, None
            vis = g.visual
            if isinstance(vis, trimesh.visual.TextureVisuals) and vis.uv is not None:
                uv = np.asarray(vis.uv, np.float64)
                mat = vis.material
                img = getattr(mat, "baseColorTexture", None) or getattr(mat, "image", None)
                if img is not None:
                    tex = np.asarray(img.convert("RGB"))
            gray = np.array([128, 128, 128], np.uint8)
            self.parts.append(_Part(np.asarray(g.faces), uv, tex, gray))
            self.scene.add_triangles(
                o3d.core.Tensor(np.asarray(g.vertices, np.float32)),
                o3d.core.Tensor(np.asarray(g.faces, np.uint32)),
            )

    def render(self, K: np.ndarray, T_wc: np.ndarray, W: int, H: int):
        """Returns (rgb uint8 HxWx3, hit bool HxW, depth float32 HxW along the optical axis)."""
        T_cw = np.linalg.inv(T_wc)
        rays = o3d.t.geometry.RaycastingScene.create_rays_pinhole(
            o3d.core.Tensor(K.astype(np.float64)), o3d.core.Tensor(T_cw), W, H
        )
        ans = self.scene.cast_rays(rays)
        t = ans["t_hit"].numpy()
        gid = ans["geometry_ids"].numpy().astype(np.int64)
        pid = ans["primitive_ids"].numpy().astype(np.int64)
        buv = ans["primitive_uvs"].numpy()
        hit = np.isfinite(t)
        rgb = np.zeros((H, W, 3), np.uint8)
        dirs = rays.numpy()[..., 3:] @ T_cw[:3, :3].T
        depth = np.where(hit, t * dirs[..., 2], 0).astype(np.float32)
        for k, part in enumerate(self.parts):
            sel = hit & (gid == k)
            if not sel.any():
                continue
            if part.uv is None or part.tex is None:
                rgb[sel] = part.gray
                continue
            f = part.faces[pid[sel]]  # (n, 3) vertex ids
            b = buv[sel]
            w = np.stack([1 - b[:, 0] - b[:, 1], b[:, 0], b[:, 1]], 1)
            uv = (part.uv[f] * w[..., None]).sum(1)
            h_, w_ = part.tex.shape[:2]
            x = uv[:, 0] * w_ - 0.5
            y = ((1 - uv[:, 1]) if self.v_up else uv[:, 1]) * h_ - 0.5
            samp = _bilinear(part.tex, x, y)
            rgb[sel] = samp
        return rgb, hit, depth


def psnr(a: np.ndarray, b: np.ndarray, mask: np.ndarray | None = None) -> float:
    d = (a.astype(np.float64) - b.astype(np.float64)) ** 2
    if mask is not None:
        d = d[mask]
    mse = float(d.mean()) if d.size else float("nan")
    return float("inf") if mse == 0 else float(10 * np.log10(255.0**2 / mse))
