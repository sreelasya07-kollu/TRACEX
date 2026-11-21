"""Synthetic rooms: textured rectangles (walls, floor, ceiling, furniture faces).

World frame: Y up, metres, floor at y=0. Deterministic for a given seed.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

TEX_PX_PER_M = 160.0


@dataclass
class Surface:
    name: str
    kind: str  # floor | ceiling | wall | object
    origin: np.ndarray  # corner (3,)
    u: np.ndarray  # unit axis along width (3,)
    v: np.ndarray  # unit axis along height (3,)
    width: float
    height: float
    texture: np.ndarray  # (th, tw, 3) float32 in [0,1]

    @property
    def normal(self) -> np.ndarray:
        n = np.cross(self.u, self.v)
        return n / np.linalg.norm(n)

    @property
    def center(self) -> np.ndarray:
        return self.origin + self.u * self.width / 2 + self.v * self.height / 2


@dataclass
class Box:
    """Axis-aligned furniture block standing on the floor."""

    x0: float
    z0: float
    x1: float
    z1: float
    h: float


@dataclass
class Room:
    width: float  # X extent
    depth: float  # Z extent
    height: float
    surfaces: list[Surface] = field(default_factory=list)
    boxes: list[Box] = field(default_factory=list)


def _texture(rng: np.random.Generator, w_m: float, h_m: float, style: str) -> np.ndarray:
    tw = max(8, int(w_m * TEX_PX_PER_M))
    th = max(8, int(h_m * TEX_PX_PER_M))
    base = rng.uniform(0.45, 0.8, size=3).astype(np.float32)
    img = np.ones((th, tw, 3), np.float32) * base
    if style == "blank":
        img[:] = 0.92
        img += rng.normal(0, 0.004, size=(th, tw, 1)).astype(np.float32)
        return np.clip(img, 0, 1)
    yy, xx = np.mgrid[0:th, 0:tw].astype(np.float32)
    n_blobs = int(w_m * h_m * 30) + 10
    for _ in range(n_blobs):
        cx, cy = rng.uniform(0, tw), rng.uniform(0, th)
        r = rng.uniform(4, 40)
        col = rng.uniform(-0.35, 0.35, size=3).astype(np.float32)
        x0, x1 = int(max(0, cx - 3 * r)), int(min(tw, cx + 3 * r))
        y0, y1 = int(max(0, cy - 3 * r)), int(min(th, cy + 3 * r))
        if x1 <= x0 or y1 <= y0:
            continue
        g = np.exp(-((xx[y0:y1, x0:x1] - cx) ** 2 + (yy[y0:y1, x0:x1] - cy) ** 2) / (2 * r * r))
        img[y0:y1, x0:x1] += g[..., None] * col
    n_rects = int(w_m * h_m * 4) + 3
    for _ in range(n_rects):
        rw, rh = rng.uniform(10, min(90, tw - 1)), rng.uniform(10, min(90, th - 1))
        x0, y0 = int(rng.uniform(0, max(1, tw - rw))), int(rng.uniform(0, max(1, th - rh)))
        img[y0 : y0 + int(rh), x0 : x0 + int(rw)] = rng.uniform(0.05, 0.95, size=3)
    if style == "floor":
        stripe = ((xx // 24).astype(int) % 2 == 0)[..., None] * 0.08
        img += stripe.astype(np.float32)
    img += rng.normal(0, 0.03, size=(th, tw, 1)).astype(np.float32)
    return np.clip(img, 0, 1)


def _rect(name, kind, origin, u, v, w, h, rng, style) -> Surface:
    return Surface(
        name=name,
        kind=kind,
        origin=np.asarray(origin, float),
        u=np.asarray(u, float),
        v=np.asarray(v, float),
        width=w,
        height=h,
        texture=_texture(rng, w, h, style),
    )


def build_room(
    width: float,
    depth: float,
    height: float,
    seed: int,
    boxes: list[Box] | None = None,
    blank_walls: bool = False,
) -> Room:
    rng = np.random.default_rng(seed)
    W, D, H = width, depth, height
    ws = "blank" if blank_walls else "wall"
    ex, ey, ez = np.eye(3)
    s = [
        # Normals point into the room (u x v).
        _rect("floor", "floor", [0, 0, 0], ez, ex, D, W, rng, "floor"),
        _rect("ceiling", "ceiling", [0, H, 0], ex, ez, W, D, rng, "blank"),
        _rect("wall_z0", "wall", [0, 0, 0], ex, ey, W, H, rng, ws),
        _rect("wall_z1", "wall", [W, 0, D], -ex, ey, W, H, rng, ws),
        _rect("wall_x0", "wall", [0, 0, D], -ez, ey, D, H, rng, ws),
        _rect("wall_x1", "wall", [W, 0, 0], ez, ey, D, H, rng, ws),
    ]
    for i, b in enumerate(boxes or []):
        bw, bd = b.x1 - b.x0, b.z1 - b.z0
        p = f"box{i}"
        s += [
            _rect(f"{p}_top", "object", [b.x0, b.h, b.z0], ez, ex, bd, bw, rng, "wall"),
            _rect(f"{p}_z0", "object", [b.x1, 0, b.z0], -ex, ey, bw, b.h, rng, "wall"),
            _rect(f"{p}_z1", "object", [b.x0, 0, b.z1], ex, ey, bw, b.h, rng, "wall"),
            _rect(f"{p}_x0", "object", [b.x0, 0, b.z0], ez, ey, bd, b.h, rng, "wall"),
            _rect(f"{p}_x1", "object", [b.x1, 0, b.z1], -ez, ey, bd, b.h, rng, "wall"),
        ]
    return Room(W, D, H, s, list(boxes or []))
