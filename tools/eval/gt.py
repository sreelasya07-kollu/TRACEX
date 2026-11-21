"""Ground-truth geometry loading (eval only).

Replica meshes store QUAD faces; open3d 0.19's PLY reader silently keeps only the triangle
faces (room0: 16,516 of 953,647 faces). We read with plyfile and split quads into 2 triangles.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import open3d as o3d
from plyfile import PlyData


def resolve_gt_path(path: str | Path, repo: Path | None = None) -> Path:
    """gt_ref.json stores the GT path as it was at ingest time. If the repo moved, re-root the
    part after `/data/` under this repo's data/ directory."""
    p = Path(path)
    if p.exists():
        return p
    repo = repo or Path(__file__).resolve().parents[1]
    parts = p.parts
    if "data" in parts:
        cand = repo / "data" / Path(*parts[len(parts) - parts[::-1].index("data") :])
        if cand.exists():
            return cand
    return p


def load_mesh(path: Path) -> o3d.geometry.TriangleMesh:
    ply = PlyData.read(str(path))
    v = ply["vertex"]
    verts = np.stack([v["x"], v["y"], v["z"]], 1).astype(np.float64)
    faces = ply["face"].data[ply["face"].properties[0].name]
    tris = []
    for n in sorted({len(f) for f in faces}):
        sel = np.stack([f for f in faces if len(f) == n]).astype(np.int64)
        if n == 3:
            tris.append(sel)
        elif n == 4:
            tris += [sel[:, [0, 1, 2]], sel[:, [0, 2, 3]]]
        else:  # fan triangulation for other polygons
            tris += [sel[:, [0, k, k + 1]] for k in range(1, n - 1)]
    m = o3d.geometry.TriangleMesh(
        o3d.utility.Vector3dVector(verts), o3d.utility.Vector3iVector(np.concatenate(tris))
    )
    names = v.data.dtype.names
    if {"red", "green", "blue"} <= set(names):
        m.vertex_colors = o3d.utility.Vector3dVector(
            np.stack([v["red"], v["green"], v["blue"]], 1) / 255.0
        )
    return m
