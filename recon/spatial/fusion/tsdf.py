"""TSDF fusion behind one interface.

backend = legacy  -> open3d.pipelines.integration.ScalableTSDFVolume (CPU)
backend = tensor  -> open3d.t.geometry.VoxelBlockGrid (device "CPU:0" or "CUDA:0")
Inputs: metric depth (float32 m, 0 = invalid), RGB uint8, K, T_world_camera.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import open3d as o3d


@dataclass
class TSDFParams:
    voxel: float = 0.02
    trunc_mult: float = 4.0  # sdf_trunc = trunc_mult * voxel
    depth_trunc: float = 8.0
    backend: str = "legacy"  # legacy | tensor
    device: str = "CPU:0"  # tensor backend only
    block_count: int = 40000

    @property
    def sdf_trunc(self) -> float:
        return self.trunc_mult * self.voxel


class TSDF:
    def __init__(self, p: TSDFParams):
        self.p = p
        if p.backend == "legacy":
            self.vol = o3d.pipelines.integration.ScalableTSDFVolume(
                voxel_length=p.voxel,
                sdf_trunc=p.sdf_trunc,
                color_type=o3d.pipelines.integration.TSDFVolumeColorType.RGB8,
            )
        else:
            import open3d.core as o3c

            self.dev = o3c.Device(p.device)
            self.vbg = o3d.t.geometry.VoxelBlockGrid(
                attr_names=("tsdf", "weight", "color"),
                attr_dtypes=(o3c.float32, o3c.float32, o3c.float32),
                attr_channels=((1), (1), (3)),
                voxel_size=p.voxel,
                block_resolution=16,
                block_count=p.block_count,
                device=self.dev,
            )
        self.n = 0

    def integrate(self, depth_m: np.ndarray, rgb: np.ndarray, K: np.ndarray, T_wc: np.ndarray):
        T_cw = np.linalg.inv(T_wc)
        depth_m = np.ascontiguousarray(depth_m, np.float32)
        rgb = np.ascontiguousarray(rgb, np.uint8)
        H, W = depth_m.shape
        if self.p.backend == "legacy":
            rgbd = o3d.geometry.RGBDImage.create_from_color_and_depth(
                o3d.geometry.Image(rgb),
                o3d.geometry.Image(depth_m),
                depth_scale=1.0,
                depth_trunc=self.p.depth_trunc,
                convert_rgb_to_intensity=False,
            )
            self.vol.integrate(
                rgbd,
                o3d.camera.PinholeCameraIntrinsic(W, H, K[0, 0], K[1, 1], K[0, 2], K[1, 2]),
                T_cw,
            )
        else:
            import open3d.core as o3c

            d = o3d.t.geometry.Image(o3c.Tensor(depth_m)).to(self.dev)
            c = o3d.t.geometry.Image(o3c.Tensor(rgb.astype(np.float32) / 255.0)).to(self.dev)
            Kt = o3c.Tensor(K, o3c.float64)
            Et = o3c.Tensor(T_cw, o3c.float64)
            blk = self.vbg.compute_unique_block_coordinates(d, Kt, Et, 1.0, self.p.depth_trunc)
            self.vbg.integrate(blk, d, c, Kt, Et, 1.0, self.p.depth_trunc)
        self.n += 1

    def extract(self) -> tuple[o3d.geometry.PointCloud, o3d.geometry.TriangleMesh]:
        if self.p.backend == "legacy":
            mesh = self.vol.extract_triangle_mesh()
            pcd = self.vol.extract_point_cloud()
        else:
            mesh = self.vbg.extract_triangle_mesh().to_legacy()
            pcd = self.vbg.extract_point_cloud().to_legacy()
        mesh.compute_vertex_normals()
        if not pcd.has_normals():
            pcd.estimate_normals(o3d.geometry.KDTreeSearchParamHybrid(3 * self.p.voxel, 30))
        return pcd, mesh
