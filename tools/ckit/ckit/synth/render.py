"""Vectorised ray-cast renderer for synthetic rooms (OpenCV camera convention)."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ckit.synth.scene import TEX_PX_PER_M, Room


@dataclass(frozen=True)
class Cam:
    w: int = 640
    h: int = 480
    fx: float = 500.0
    fy: float = 500.0
    cx: float = 319.5
    cy: float = 239.5


@dataclass
class Hits:
    rgb: np.ndarray  # (h, w, 3) float32
    xyz: np.ndarray  # (h, w, 3) world points (nan where no hit)
    surf: np.ndarray  # (h, w) int surface index, -1 none
    depth: np.ndarray  # (h, w) z in camera frame


def _rays(cam: Cam) -> np.ndarray:
    u, v = np.meshgrid(np.arange(cam.w, dtype=np.float64), np.arange(cam.h, dtype=np.float64))
    d = np.stack([(u - cam.cx) / cam.fx, (v - cam.cy) / cam.fy, np.ones_like(u)], -1)
    return d  # camera-frame directions with z = 1


def render(room: Room, cam: Cam, R_wc: np.ndarray, t_wc: np.ndarray, light: float = 1.0) -> Hits:
    """Render the room from pose T_world_from_camera = (R_wc, t_wc)."""
    dc = _rays(cam).reshape(-1, 3)
    dw = dc @ R_wc.T
    n_px = dw.shape[0]
    best_t = np.full(n_px, np.inf)
    best_s = np.full(n_px, -1, np.int32)
    best_a = np.zeros(n_px)
    best_b = np.zeros(n_px)
    for i, s in enumerate(room.surfaces):
        n = s.normal
        denom = dw @ n
        with np.errstate(divide="ignore", invalid="ignore"):
            t = ((s.origin - t_wc) @ n) / denom
        ok = (denom < -1e-9) & (t > 1e-4) & (t < best_t)  # front faces only
        if not ok.any():
            continue
        idx = np.nonzero(ok)[0]
        p = t_wc + dw[idx] * t[idx, None] - s.origin
        a = p @ s.u
        b = p @ s.v
        inside = (a >= 0) & (a <= s.width) & (b >= 0) & (b <= s.height)
        idx = idx[inside]
        best_t[idx] = t[idx]
        best_s[idx] = i
        best_a[idx] = a[inside]
        best_b[idx] = b[inside]

    rgb = np.zeros((n_px, 3), np.float32)
    for i, s in enumerate(room.surfaces):
        m = best_s == i
        if not m.any():
            continue
        tex = s.texture
        th, tw = tex.shape[:2]
        x = np.clip(best_a[m] * TEX_PX_PER_M - 0.5, 0, tw - 1.001)
        y = np.clip((s.height - best_b[m]) * TEX_PX_PER_M - 0.5, 0, th - 1.001)
        x0, y0 = x.astype(int), y.astype(int)
        fx, fy = (x - x0)[:, None], (y - y0)[:, None]
        c = (
            tex[y0, x0] * (1 - fx) * (1 - fy)
            + tex[y0, x0 + 1] * fx * (1 - fy)
            + tex[y0 + 1, x0] * (1 - fx) * fy
            + tex[y0 + 1, x0 + 1] * fx * fy
        )
        shade = {"floor": 0.85, "ceiling": 1.0, "wall": 0.95, "object": 0.9}[s.kind]
        shade *= 0.75 + 0.25 * abs(float(s.normal @ np.array([0.3, 0.8, 0.5])))
        rgb[m] = c * shade * light
    xyz = np.full((n_px, 3), np.nan)
    hit = best_s >= 0
    xyz[hit] = t_wc + dw[hit] * best_t[hit, None]
    depth = np.where(hit, best_t, np.nan)  # dc has z=1, so t == camera z
    return Hits(
        rgb=rgb.reshape(cam.h, cam.w, 3),
        xyz=xyz.reshape(cam.h, cam.w, 3),
        surf=best_s.reshape(cam.h, cam.w),
        depth=depth.reshape(cam.h, cam.w),
    )
