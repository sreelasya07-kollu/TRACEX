"""Standard monocular-depth metrics (Eigen et al. 2014), as used by Depth-Anything-V2 / ZoeDepth.

Metrics are computed per image on valid GT pixels inside [min_depth, max_depth] and then
averaged over images (the protocol used for published NYU numbers).
"""

from __future__ import annotations

import numpy as np

KEYS = ("abs_rel", "sq_rel", "rmse", "rmse_log", "d1", "d2", "d3")


def depth_metrics(
    pred: np.ndarray,
    gt: np.ndarray,
    mask: np.ndarray | None = None,
    min_depth: float = 1e-3,
    max_depth: float = 10.0,
) -> dict[str, float]:
    """pred, gt in metres, same shape. Returns a dict of the 7 standard metrics + n_pixels."""
    pred = np.asarray(pred, np.float64)
    gt = np.asarray(gt, np.float64)
    valid = np.isfinite(gt) & (gt > min_depth) & (gt < max_depth) & np.isfinite(pred)
    if mask is not None:
        valid &= mask.astype(bool)
    p = np.clip(pred[valid], min_depth, max_depth)
    g = gt[valid]
    if g.size == 0:
        return {k: float("nan") for k in KEYS} | {"n_pixels": 0}
    thresh = np.maximum(g / p, p / g)
    return {
        "abs_rel": float(np.mean(np.abs(g - p) / g)),
        "sq_rel": float(np.mean((g - p) ** 2 / g)),
        "rmse": float(np.sqrt(np.mean((g - p) ** 2))),
        "rmse_log": float(np.sqrt(np.mean((np.log(g) - np.log(p)) ** 2))),
        "d1": float(np.mean(thresh < 1.25)),
        "d2": float(np.mean(thresh < 1.25**2)),
        "d3": float(np.mean(thresh < 1.25**3)),
        "n_pixels": int(g.size),
    }


def aggregate(per_image: list[dict]) -> dict[str, float]:
    """Mean over images (images without valid pixels are skipped)."""
    rows = [m for m in per_image if m.get("n_pixels", 0) > 0]
    return {k: float(np.mean([m[k] for m in rows])) for k in KEYS} | {"n_images": len(rows)}
