"""Small geometry helpers (poses, quaternions, Umeyama). Conventions: docs/INTERFACES.md §0."""

from __future__ import annotations

import numpy as np
from scipy.spatial.transform import Rotation


def pose_from_tq_xyzw(t, q_xyzw) -> np.ndarray:
    """4x4 from translation + quaternion in (x, y, z, w) order (TUM, scipy)."""
    T = np.eye(4)
    T[:3, :3] = Rotation.from_quat(q_xyzw).as_matrix()  # scipy uses scalar-last
    T[:3, 3] = t
    return T


def invert(T: np.ndarray) -> np.ndarray:
    Ti = np.eye(4)
    Ti[:3, :3] = T[:3, :3].T
    Ti[:3, 3] = -T[:3, :3].T @ T[:3, 3]
    return Ti


def umeyama(src: np.ndarray, dst: np.ndarray, with_scale: bool = True):
    """Least-squares s, R, t with dst ≈ s·R·src + t (Umeyama 1991). src, dst: (N, 3)."""
    mu_s, mu_d = src.mean(0), dst.mean(0)
    xs, xd = src - mu_s, dst - mu_d
    cov = xd.T @ xs / len(src)
    U, D, Vt = np.linalg.svd(cov)
    S = np.eye(3)
    if np.linalg.det(U) * np.linalg.det(Vt) < 0:
        S[2, 2] = -1
    R = U @ S @ Vt
    var_s = (xs**2).sum() / len(src)
    s = (D * np.diag(S)).sum() / var_s if with_scale else 1.0
    t = mu_d - s * R @ mu_s
    return s, R, t


def rotation_aligning(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Smallest rotation R with R @ a ∥ b (unit vectors)."""
    a, b = a / np.linalg.norm(a), b / np.linalg.norm(b)
    v, c = np.cross(a, b), float(a @ b)
    if np.linalg.norm(v) < 1e-12:
        if c > 0:
            return np.eye(3)
        axis = np.cross(a, [1, 0, 0] if abs(a[0]) < 0.9 else [0, 1, 0])
        return Rotation.from_rotvec(np.pi * axis / np.linalg.norm(axis)).as_matrix()
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1 / (1 + c))
