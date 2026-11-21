"""Align predicted depth to the metric sparse SfM model (per frame, RANSAC + Huber).

Observations: the 3D points a frame actually observed in COLMAP (its points2D with a
point3D id), so occluded points are never used. For each: z = camera depth of the 3D
point (metres), p = prediction sampled bilinearly at the observed keypoint.
  kind = metric            z = s * p                       (1-point RANSAC)
  kind = inverse_relative  1/z = s * p + t, depth = 1/(s*p+t) (2-point RANSAC, inverse space)
Residuals are relative depth errors |z_hat - z| / z.
AlignParams.method: "ransac" (default, RANSAC + Huber) or "lstsq" (plain unweighted least
squares baseline, no outlier handling; same inlier/status rules for comparability).
"""

from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np
from scipy.ndimage import median_filter
from scipy.optimize import least_squares


@dataclass
class AlignParams:
    ransac_iters: int = 200
    inlier_rel: float = 0.05  # relative depth residual for RANSAC inliers
    min_inliers: int = 20
    max_residual_rel: float = 0.10  # reject frame if median inlier residual exceeds this
    huber_rel: float = 0.02
    smooth_window: int = 5  # temporal median over log-scale (odd); 1 = off
    seed: int = 0
    method: str = "ransac"  # "ransac" | "lstsq" (plain least-squares baseline)

    def __post_init__(self):
        if self.method not in ("ransac", "lstsq"):
            raise ValueError(f"AlignParams.method must be 'ransac' or 'lstsq', got {self.method!r}")


def sample_bilinear(img: np.ndarray, uv: np.ndarray) -> np.ndarray:
    m = cv2.remap(
        img.astype(np.float32),
        uv[:, 0:1].astype(np.float32),
        uv[:, 1:2].astype(np.float32),
        cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_CONSTANT,
        borderValue=np.nan,
    )
    return m[:, 0]


def _predict(kind, s, t, p):
    if kind == "metric":
        return s * p
    with np.errstate(divide="ignore"):
        return 1.0 / (s * p + t)


def _fit_lstsq(z: np.ndarray, p: np.ndarray, kind: str, prm: AlignParams, n: int) -> dict:
    """Plain unweighted OLS baseline (no RANSAC, no Huber, no relative weighting).

    metric: s = sum(p*z) / sum(p*p), t = 0. inverse_relative: OLS of 1/z on [p, 1].
    Inliers / residual / status use the same rules as the RANSAC path, on all points.
    """
    res = {"scale": None, "shift": None, "inliers": 0, "n_points": int(n), "residual_m": None}
    if kind == "metric":
        s, t = float(np.sum(p * z) / np.sum(p * p)), 0.0
    else:
        sol, _, rank, _ = np.linalg.lstsq(np.c_[p, np.ones(n)], 1.0 / z, rcond=None)
        if rank < 2:
            return res | {"status": "rejected_few_points"}
        s, t = float(sol[0]), float(sol[1])
    if not (np.isfinite(s) and np.isfinite(t)):
        return res | {"status": "rejected_few_points"}
    with np.errstate(invalid="ignore", divide="ignore"):
        zh = _predict(kind, s, t, p)
        inl = np.abs(zh - z) / z < prm.inlier_rel
    resid = float(np.median(np.abs(zh[inl] - z[inl]))) if inl.any() else None
    rel = float(np.median(np.abs(zh[inl] - z[inl]) / z[inl])) if inl.any() else np.inf
    status = (
        "ok"
        if inl.sum() >= prm.min_inliers and rel <= prm.max_residual_rel
        else ("rejected_few_points" if inl.sum() < prm.min_inliers else "rejected_residual")
    )
    return {
        "scale": s,
        "shift": t,
        "inliers": int(inl.sum()),
        "n_points": int(n),
        "residual_m": resid,
        "status": status,
    }


def fit_scale(z: np.ndarray, p: np.ndarray, kind: str, prm: AlignParams) -> dict:
    """Fit depth = f(p; s, t) to metric z (robust unless prm.method == "lstsq").

    Returns s, t, inlier mask, residual.
    """
    ok = np.isfinite(z) & np.isfinite(p) & (z > 0) & (p > 0)
    z, p = z[ok], p[ok]
    n = len(z)
    res = {"scale": None, "shift": None, "inliers": 0, "n_points": int(n), "residual_m": None}
    need = 1 if kind == "metric" else 2
    if n < max(need, prm.min_inliers):
        return res | {"status": "rejected_few_points"}
    if prm.method == "lstsq":
        return _fit_lstsq(z, p, kind, prm, n)
    rng = np.random.default_rng(prm.seed)
    best = (-1, None, None)
    for _ in range(prm.ransac_iters):
        idx = rng.choice(n, need, replace=False)
        if kind == "metric":
            s, t = float(z[idx[0]] / p[idx[0]]), 0.0
        else:
            A = np.c_[p[idx], np.ones(2)]
            if abs(np.linalg.det(A)) < 1e-12:
                continue
            s, t = np.linalg.solve(A, 1.0 / z[idx])
        zh = _predict(kind, s, t, p)
        inl = np.abs(zh - z) / z < prm.inlier_rel
        if inl.sum() > best[0]:
            best = (int(inl.sum()), s, t)
    _, s0, t0 = best
    inl = np.abs(_predict(kind, s0, t0, p) - z) / z < prm.inlier_rel
    if inl.sum() < prm.min_inliers:
        return res | {"inliers": int(inl.sum()), "status": "rejected_few_points"}

    zi, pi = z[inl], p[inl]
    if kind == "metric":
        sol = least_squares(
            lambda x: (x[0] * pi - zi) / zi, [s0], loss="huber", f_scale=prm.huber_rel
        )
        s, t = float(sol.x[0]), 0.0
    else:
        sol = least_squares(
            lambda x: (_predict(kind, x[0], x[1], pi) - zi) / zi,
            [s0, t0],
            loss="huber",
            f_scale=prm.huber_rel,
        )
        s, t = float(sol.x[0]), float(sol.x[1])
    zh = _predict(kind, s, t, p)
    inl = np.abs(zh - z) / z < prm.inlier_rel
    resid = float(np.median(np.abs(zh[inl] - z[inl]))) if inl.any() else None
    rel = float(np.median(np.abs(zh[inl] - z[inl]) / z[inl])) if inl.any() else np.inf
    status = (
        "ok"
        if inl.sum() >= prm.min_inliers and rel <= prm.max_residual_rel
        else ("rejected_few_points" if inl.sum() < prm.min_inliers else "rejected_residual")
    )
    return {
        "scale": s,
        "shift": t,
        "inliers": int(inl.sum()),
        "n_points": int(n),
        "residual_m": resid,
        "status": status,
    }


def smooth_scales(scales: list[float | None], window: int) -> list[float | None]:
    """Temporal median over log-scale of accepted frames (in frame order)."""
    idx = [i for i, s in enumerate(scales) if s is not None and s > 0]
    out = list(scales)
    if window <= 1 or len(idx) < 2:
        return out
    logs = np.log([scales[i] for i in idx])
    sm = np.exp(median_filter(logs, size=window, mode="nearest"))
    for i, v in zip(idx, sm, strict=True):
        out[i] = float(v)
    return out


def apply_alignment(pred: np.ndarray, kind: str, s: float, t: float) -> np.ndarray:
    d = _predict(kind, s, t, pred).astype(np.float32)
    d[~np.isfinite(d) | (d <= 0)] = 0.0
    return d
