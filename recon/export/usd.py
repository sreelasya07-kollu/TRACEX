"""Room shell as ASCII USD (.usda): floor + one wall quad per floor-polygon edge + ceiling.
Pure text (no pxr needed); convert to .usdz with Apple's/OpenUSD `usdzip` where available."""

from __future__ import annotations

from pathlib import Path

import numpy as np

from export.geometry import triangulate
from formats.spatial_map import SpatialMap


def room_shell_mesh(sm: SpatialMap) -> tuple[np.ndarray, list[int], list[int]]:
    """(points (N,3), faceVertexCounts, faceVertexIndices); +Z up, metres."""
    p = np.asarray(sm.room.floor_polygon, float)
    r = sm.room
    if len(p) < 3 or r.floor_z is None:
        raise ValueError("room shell needs a floor polygon and a floor height")
    z0 = r.floor_z
    z1 = r.ceiling_z if r.ceiling_z is not None else z0 + 2.5
    n = len(p)
    pts = np.vstack([np.c_[p, np.full(n, z0)], np.c_[p, np.full(n, z1)]])
    counts, idx = [], []
    for a, b, c in triangulate(p):  # floor, normal +Z (CCW from above)
        counts.append(3)
        idx += [a, b, c]
    for a, b, c in triangulate(p):  # ceiling, normal -Z
        counts.append(3)
        idx += [n + a, n + c, n + b]
    for i in range(n):  # walls, normals pointing into the room (left of a CCW edge)
        j = (i + 1) % n
        counts.append(4)
        idx += [i, n + i, n + j, j]
    return pts, counts, idx


def write_usda(sm: SpatialMap, path: Path) -> dict:
    pts, counts, idx = room_shell_mesh(sm)
    f = lambda v: f"({v[0]:.4f}, {v[1]:.4f}, {v[2]:.4f})"  # noqa: E731
    text = f"""#usda 1.0
(
    defaultPrim = "Room"
    metersPerUnit = 1
    upAxis = "Z"
    doc = "Room shell from a SpatialMap; scale_confidence={sm.room.scale_confidence}"
)

def Xform "Room"
{{
    def Mesh "Shell"
    {{
        int[] faceVertexCounts = [{", ".join(map(str, counts))}]
        int[] faceVertexIndices = [{", ".join(map(str, idx))}]
        point3f[] points = [{", ".join(f(q) for q in pts)}]
        uniform token subdivisionScheme = "none"
        bool doubleSided = 1
    }}
}}
"""
    path.write_text(text)
    return {"points": len(pts), "faces": len(counts)}
