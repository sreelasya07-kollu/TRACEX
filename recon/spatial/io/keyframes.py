"""Keyframe selection (HANDOFF P1): drop motion-blurred frames (variance of the Laplacian) and
keep the sharpest frame of every motion group, so consecutive keyframes are never far apart.

Groups are runs of consecutive frames whose prior pose stays within `group_trans_m` /
`group_rot_deg` of the group's first frame; one keyframe per group, so the gap between two
consecutive keyframes is bounded by ~2 x the group size plus one frame step. Without poses
(prior = none) only the blur filter applies. Poses are the PRIORS (ARKit-like), never GT."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
from scipy.spatial.transform import Rotation


@dataclass
class KeyframeParams:
    min_sharp_ratio: float = 0.5  # frames below ratio x median sharpness count as blurred
    group_trans_m: float = 0.12
    group_rot_deg: float = 6.0


def sharpness(image_path: Path, max_side: int = 640) -> float:
    g = cv2.imread(str(image_path), cv2.IMREAD_GRAYSCALE)
    s = max_side / max(g.shape)
    if s < 1:
        g = cv2.resize(g, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)
    return float(cv2.Laplacian(g, cv2.CV_64F).var())


def _rot_deg(Ra: np.ndarray, Rb: np.ndarray) -> float:
    return float(np.degrees(Rotation.from_matrix(Ra.T @ Rb).magnitude()))


def select_keyframes(
    sharp: np.ndarray, poses: list[np.ndarray | None], p: KeyframeParams | None = None
) -> list[int]:
    p = p or KeyframeParams()
    sharp = np.asarray(sharp, float)
    ok = sharp >= p.min_sharp_ratio * np.median(sharp)
    if any(T is None for T in poses):
        return [i for i in range(len(sharp)) if ok[i]] or [int(np.argmax(sharp))]
    keys: list[int] = []
    start = 0
    n = len(sharp)
    for i in range(1, n + 1):
        end_group = i == n
        if not end_group:
            d = np.linalg.norm(poses[i][:3, 3] - poses[start][:3, 3])
            end_group = (
                d > p.group_trans_m
                or _rot_deg(poses[start][:3, :3], poses[i][:3, :3]) > p.group_rot_deg
            )
        if end_group:
            idx = np.arange(start, i)
            cand = idx[ok[idx]]
            keys.append(
                int((cand if len(cand) else idx)[np.argmax(sharp[cand if len(cand) else idx])])
            )
            start = i
    return keys


def max_gap(poses: list[np.ndarray]) -> tuple[float, float]:
    """Largest translation [m] and rotation [deg] between consecutive poses."""
    if len(poses) < 2:
        return 0.0, 0.0
    t = [float(np.linalg.norm(b[:3, 3] - a[:3, 3])) for a, b in zip(poses, poses[1:], strict=False)]
    r = [_rot_deg(a[:3, :3], b[:3, :3]) for a, b in zip(poses, poses[1:], strict=False)]
    return max(t), max(r)
