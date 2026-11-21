"""Point-cloud cleanup: statistical + radius outlier removal, voxel downsample."""

from __future__ import annotations

from dataclasses import dataclass

import open3d as o3d


@dataclass
class CleanParams:
    voxel: float | None = None  # None = no downsample
    stat_nb: int = 20
    stat_std: float = 2.0
    radius_mult: float = 2.5  # radius = radius_mult * fusion voxel
    radius_min_pts: int = 6
    enabled: bool = True


def clean(pcd: o3d.geometry.PointCloud, fusion_voxel: float, p: CleanParams):
    stats = {"input": len(pcd.points)}
    if not p.enabled:
        return pcd, stats | {"output": len(pcd.points)}
    if p.voxel:
        pcd = pcd.voxel_down_sample(p.voxel)
        stats["after_voxel"] = len(pcd.points)
    pcd, _ = pcd.remove_statistical_outlier(p.stat_nb, p.stat_std)
    stats["after_statistical"] = len(pcd.points)
    pcd, _ = pcd.remove_radius_outlier(p.radius_min_pts, p.radius_mult * fusion_voxel)
    stats["output"] = len(pcd.points)
    return pcd, stats
