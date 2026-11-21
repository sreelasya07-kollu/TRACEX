"""2D polygon helpers for exports (simple polygons, CCW)."""

from __future__ import annotations

import numpy as np


def area(poly: np.ndarray) -> float:
    x, y = poly[:, 0], poly[:, 1]
    return 0.5 * float(np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1)))


def edge_lengths(poly: np.ndarray) -> list[float]:
    return [float(np.linalg.norm(b - a)) for a, b in zip(poly, np.roll(poly, -1, 0), strict=True)]


def triangulate(poly: np.ndarray) -> list[tuple[int, int, int]]:
    """Ear clipping of a simple CCW polygon -> vertex-index triangles (handles L/U shapes)."""
    n = len(poly)
    idx = list(range(n))
    tris: list[tuple[int, int, int]] = []

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    def inside(p, a, b, c):
        return cross(a, b, p) >= -1e-12 and cross(b, c, p) >= -1e-12 and cross(c, a, p) >= -1e-12

    guard = 0
    while len(idx) > 3 and guard < 10 * n * n:
        guard += 1
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if cross(a, b, c) <= 1e-12:  # reflex or degenerate corner
                continue
            if any(inside(poly[j], a, b, c) for j in idx if j not in (i0, i1, i2)):
                continue
            tris.append((i0, i1, i2))
            idx.pop(k)
            break
        else:
            raise ValueError("polygon is not simple / cannot be triangulated")
    tris.append((idx[0], idx[1], idx[2]))
    return tris
