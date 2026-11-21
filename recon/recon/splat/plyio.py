"""Standard 3D-Gaussian-Splatting PLY (INRIA layout) read/write. Attribute order:
x y z nx ny nz f_dc_0..2 f_rest_0..(3K-1) opacity scale_0..2 rot_0..3.
f_rest is channel-major (all R coefficients, then G, then B) as in the reference code."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from plyfile import PlyData, PlyElement


def _names(k_rest: int) -> list[str]:
    return (
        ["x", "y", "z", "nx", "ny", "nz"]
        + [f"f_dc_{i}" for i in range(3)]
        + [f"f_rest_{i}" for i in range(3 * k_rest)]
        + ["opacity"]
        + [f"scale_{i}" for i in range(3)]
        + [f"rot_{i}" for i in range(4)]
    )


def write_gaussian_ply(path: Path, p: dict[str, np.ndarray]) -> None:
    """p: means (N,3), scales (N,3) log, quats (N,4) wxyz, opacities (N,) logit, sh0 (N,1,3),
    shN (N,K,3)."""
    n = len(p["means"])
    k = p["shN"].shape[1]
    rest = p["shN"].transpose(0, 2, 1).reshape(n, 3 * k)  # channel-major
    cols = np.concatenate(
        [
            p["means"],
            np.zeros((n, 3)),
            p["sh0"].reshape(n, 3),
            rest,
            p["opacities"].reshape(n, 1),
            p["scales"],
            p["quats"],
        ],
        1,
    ).astype(np.float32)
    names = _names(k)
    el = np.empty(n, dtype=[(nm, "<f4") for nm in names])
    for i, nm in enumerate(names):
        el[nm] = cols[:, i]
    PlyData([PlyElement.describe(el, "vertex")], text=False).write(str(path))


def read_gaussian_ply(path: Path) -> dict[str, np.ndarray]:
    v = PlyData.read(str(path))["vertex"]
    names = v.data.dtype.names
    k3 = len([n for n in names if n.startswith("f_rest_")])
    n = len(v.data)
    g = lambda keys: np.stack([v[kk] for kk in keys], 1).astype(np.float32)  # noqa: E731
    k = k3 // 3
    rest = g([f"f_rest_{i}" for i in range(k3)]).reshape(n, 3, k).transpose(0, 2, 1)
    return {
        "means": g(["x", "y", "z"]),
        "sh0": g([f"f_dc_{i}" for i in range(3)]).reshape(n, 1, 3),
        "shN": rest,
        "opacities": np.asarray(v["opacity"], np.float32),
        "scales": g([f"scale_{i}" for i in range(3)]),
        "quats": g([f"rot_{i}" for i in range(4)]),
    }
