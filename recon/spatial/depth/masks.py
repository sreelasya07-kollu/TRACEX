"""Per-pixel validity masks for metric depth maps (True = keep)."""

from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np


@dataclass
class MaskParams:
    min_m: float = 0.1
    max_m: float = 8.0
    edge_rel: float = 0.05  # max relative depth jump to a 4-neighbour
    edge_dilate: int = 1  # pixels
    grazing_deg: float = 75.0  # max angle between surface normal and view ray
    conf_min: int | None = None  # sensor confidence threshold (uint8)


def range_mask(d: np.ndarray, p: MaskParams) -> np.ndarray:
    return (d > p.min_m) & (d < p.max_m)


def edge_mask(d: np.ndarray, p: MaskParams) -> np.ndarray:
    """Remove depth discontinuities (flying pixels) and a small band around them."""
    jump = np.zeros(d.shape, bool)
    for dy, dx in ((0, 1), (1, 0)):
        a, b = d[: d.shape[0] - dy, : d.shape[1] - dx], d[dy:, dx:]
        rel = np.abs(a - b) / np.maximum(np.minimum(a, b), 1e-6)
        j = (rel > p.edge_rel) & (a > 0) & (b > 0)
        jump[: d.shape[0] - dy, : d.shape[1] - dx] |= j
        jump[dy:, dx:] |= j
    if p.edge_dilate > 0:
        k = np.ones((2 * p.edge_dilate + 1,) * 2, np.uint8)
        jump = cv2.dilate(jump.astype(np.uint8), k) > 0
    return ~jump


def normals_from_depth(d: np.ndarray, K: np.ndarray) -> np.ndarray:
    """Camera-frame normals from central differences of the back-projected depth map."""
    H, W = d.shape
    u, v = np.meshgrid(np.arange(W), np.arange(H))
    X = np.stack([(u - K[0, 2]) * d / K[0, 0], (v - K[1, 2]) * d / K[1, 1], d], -1)
    # np.gradient: central differences inside, one-sided at the borders (no undefined normals)
    dy, dx = np.gradient(X, axis=(0, 1))
    n = np.cross(dx, dy)
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-12)
    return n


def grazing_mask(d: np.ndarray, K: np.ndarray, p: MaskParams) -> np.ndarray:
    H, W = d.shape
    u, v = np.meshgrid(np.arange(W), np.arange(H))
    ray = np.stack([(u - K[0, 2]) / K[0, 0], (v - K[1, 2]) / K[1, 1], np.ones_like(d)], -1)
    ray /= np.linalg.norm(ray, axis=-1, keepdims=True)
    cosang = np.abs(np.sum(normals_from_depth(d, K) * ray, -1))
    return cosang > np.cos(np.radians(p.grazing_deg))


def build_mask(
    d: np.ndarray,
    K: np.ndarray,
    p: MaskParams,
    conf: np.ndarray | None = None,
    enabled: bool = True,
) -> tuple[np.ndarray, dict]:
    """Returns (mask, fraction removed per filter). With enabled=False only range applies."""
    m = range_mask(d, p)
    stats = {"range": float(1 - m.mean())}
    if enabled:
        for name, mm in (("edge", edge_mask(d, p)), ("grazing", grazing_mask(d, K, p))):
            stats[name] = float((m & ~mm).mean())
            m &= mm
        if conf is not None and p.conf_min is not None:
            mm = conf >= p.conf_min
            stats["confidence"] = float((m & ~mm).mean())
            m &= mm
    return m, stats
