"""Point-cloud export: LAZ (laspy + lazrs) with 1 mm resolution and 16-bit colour.
E57 is not produced (no maintained pure-Python writer): convert LAZ with CloudCompare/PDAL."""

from __future__ import annotations

from pathlib import Path

import laspy
import numpy as np
import open3d as o3d


def write_laz(ply: Path, out: Path, scale: float = 0.001) -> int:
    pcd = o3d.io.read_point_cloud(str(ply))
    xyz = np.asarray(pcd.points)
    hdr = laspy.LasHeader(point_format=2, version="1.2")
    hdr.offsets = xyz.min(0)
    hdr.scales = [scale] * 3
    las = laspy.LasData(hdr)
    las.x, las.y, las.z = xyz[:, 0], xyz[:, 1], xyz[:, 2]
    if pcd.has_colors():
        c = (np.asarray(pcd.colors) * 65535).astype(np.uint16)
        las.red, las.green, las.blue = c[:, 0], c[:, 1], c[:, 2]
    las.write(str(out))
    return len(xyz)


def read_laz_xyz(path: Path) -> np.ndarray:
    las = laspy.read(str(path))
    return np.stack([las.x, las.y, las.z], 1)
