"""Trajectory metrics. Poses are T_world_camera (4x4) keyed by frame id."""

from __future__ import annotations

import numpy as np

from spatial.geometry import invert, umeyama


def _rot_err_deg(Ra: np.ndarray, Rb: np.ndarray) -> float:
    c = (np.trace(Ra.T @ Rb) - 1) / 2
    return float(np.degrees(np.arccos(np.clip(c, -1, 1))))


def ate(est: dict[str, np.ndarray], gt: dict[str, np.ndarray], align: str = "none") -> dict:
    """Absolute trajectory error on camera centres.

    align = none  -> est is assumed to already be in the GT frame (measures absolute error)
            se3   -> rigid Umeyama alignment
            sim3  -> similarity alignment (reports the scale factor)
    """
    keys = sorted(set(est) & set(gt))
    P = np.array([est[k][:3, 3] for k in keys])
    G = np.array([gt[k][:3, 3] for k in keys])
    s, R, t = 1.0, np.eye(3), np.zeros(3)
    if align in ("se3", "sim3"):
        s, R, t = umeyama(P, G, with_scale=align == "sim3")
    P2 = s * P @ R.T + t
    e = np.linalg.norm(P2 - G, axis=1)
    rot = [_rot_err_deg(R @ est[k][:3, :3], gt[k][:3, :3]) for k in keys]
    return {
        "n": len(keys),
        "align": align,
        "scale": float(s),
        "rmse_m": float(np.sqrt((e**2).mean())),
        "median_m": float(np.median(e)),
        "max_m": float(e.max()),
        "rot_mean_deg": float(np.mean(rot)),
        "rot_max_deg": float(np.max(rot)),
    }


def rpe(est: dict[str, np.ndarray], gt: dict[str, np.ndarray], delta: int = 1) -> dict:
    """Relative pose error between frames `delta` apart in sorted-id order (drift per step)."""
    keys = sorted(set(est) & set(gt))
    te, re = [], []
    for a, b in zip(keys[:-delta], keys[delta:], strict=True):
        dE = invert(est[a]) @ est[b]
        dG = invert(gt[a]) @ gt[b]
        E = invert(dG) @ dE
        te.append(np.linalg.norm(E[:3, 3]))
        re.append(_rot_err_deg(np.eye(3), E[:3, :3]))
    return {
        "delta": delta,
        "trans_rmse_m": float(np.sqrt(np.mean(np.square(te)))),
        "rot_rmse_deg": float(np.sqrt(np.mean(np.square(re)))),
    }
