"""Multi-view geometric consistency filter (vectorised numpy).

A pixel of frame i is kept if, reprojected into >= min_views of its K neighbours, its
depth agrees with that neighbour's depth map within `rel_thresh` (relative). Neighbours
are chosen by pose overlap: nearest camera centres whose viewing directions differ by
less than `max_angle_deg`. Pixels that project outside every neighbour count as
unverifiable; `keep_unverifiable` decides their fate (default: drop).
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass
class ConsistencyParams:
    k_neighbours: int = 4
    min_views: int = 2
    rel_thresh: float = 0.03
    max_angle_deg: float = 60.0
    min_baseline_m: float = 0.02
    keep_unverifiable: bool = False


def select_neighbours(T_wc: list[np.ndarray], p: ConsistencyParams) -> list[list[int]]:
    C = np.array([T[:3, 3] for T in T_wc])
    Z = np.array([T[:3, 2] for T in T_wc])
    out = []
    for i in range(len(T_wc)):
        d = np.linalg.norm(C - C[i], axis=1)
        ang = np.degrees(np.arccos(np.clip(Z @ Z[i], -1, 1)))
        cand = [
            j
            for j in np.argsort(d)
            if j != i and ang[j] < p.max_angle_deg and d[j] >= p.min_baseline_m
        ]
        out.append(cand[: p.k_neighbours])
    return out


def consistency_mask(
    i: int,
    depths: list[np.ndarray],
    valid: list[np.ndarray],
    T_wc: list[np.ndarray],
    K: np.ndarray,
    neigh: list[int],
    p: ConsistencyParams,
) -> tuple[np.ndarray, dict]:
    d = depths[i]
    H, W = d.shape
    v, u = np.nonzero(valid[i])
    z = d[v, u]
    Xc = np.stack([(u - K[0, 2]) * z / K[0, 0], (v - K[1, 2]) * z / K[1, 1], z], 1)
    Xw = Xc @ T_wc[i][:3, :3].T + T_wc[i][:3, 3]
    agree = np.zeros(len(z), np.int32)
    seen = np.zeros(len(z), np.int32)
    for j in neigh:
        T_cw = np.linalg.inv(T_wc[j])
        Y = Xw @ T_cw[:3, :3].T + T_cw[:3, 3]
        zj = Y[:, 2]
        with np.errstate(divide="ignore", invalid="ignore"):
            uj = np.round(Y[:, 0] / zj * K[0, 0] + K[0, 2]).astype(np.int64)
            vj = np.round(Y[:, 1] / zj * K[1, 1] + K[1, 2]).astype(np.int64)
        inside = (zj > 0) & (uj >= 0) & (uj < W) & (vj >= 0) & (vj < H)
        dj = np.zeros(len(z), np.float32)
        okj = np.zeros(len(z), bool)
        dj[inside] = depths[j][vj[inside], uj[inside]]
        okj[inside] = valid[j][vj[inside], uj[inside]]
        test = inside & okj & (dj > 0)
        seen += test
        agree += test & (np.abs(zj - dj) / dj < p.rel_thresh)
    keep = agree >= p.min_views
    if p.keep_unverifiable:
        keep |= seen < p.min_views
    m = np.zeros((H, W), bool)
    m[v[keep], u[keep]] = True
    stats = {
        "input": int(len(z)),
        "kept": int(keep.sum()),
        "unverifiable": int((seen < p.min_views).sum()),
    }
    return m, stats
