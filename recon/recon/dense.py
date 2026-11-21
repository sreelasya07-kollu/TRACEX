"""Phase 8: dense point cloud -> mesh -> texture, via OpenMVS (docker, CPU), with an Open3D
Poisson fallback for the mesh when OpenMVS is unavailable. Outputs (HANDOFF §4.3):

  <out>/dense/scene_dense.{mvs,ply}
  <out>/mesh/mesh.ply [+ mesh_refined.ply]
  <out>/mesh/textured/mesh.obj|mtl|*.png|jpg   (OpenMVS TextureMesh)
  <out>/mesh/mesh.glb                          (trimesh export of the textured mesh)
"""

from __future__ import annotations

import time
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
import open3d as o3d

from recon import openmvs
from recon.sparse import SparseScene, export_for_openmvs


@dataclass
class DenseParams:
    resolution_level: int = 1  # images scaled down 2^level for patch-match / texture
    number_views: int = 5
    max_threads: int = 4
    min_resolution: int = 320
    decimate: float = 1.0
    refine: bool = False  # RefineMesh is slow on CPU; off by default
    texture: bool = True
    poisson_depth: int = 9  # fallback only
    timeout_s: int = 7200
    memory_gb: float | None = None  # docker memory cap per OpenMVS step (RAM + swap)


def _newest(folder: Path, pattern: str) -> Path:
    c = sorted(folder.glob(pattern), key=lambda p: p.stat().st_mtime)
    if not c:
        raise FileNotFoundError(f"{folder}/{pattern}")
    return c[-1]


def poisson_mesh(dense_ply: Path, out_ply: Path, depth: int = 9) -> dict:
    """CPU fallback: screened Poisson on the dense cloud, trimmed at the 5 % density quantile."""
    pcd = o3d.io.read_point_cloud(str(dense_ply))
    if not pcd.has_normals():
        pcd.estimate_normals(o3d.geometry.KDTreeSearchParamHybrid(0.1, 30))
        pcd.orient_normals_consistent_tangent_plane(15)
    mesh, dens = o3d.geometry.TriangleMesh.create_from_point_cloud_poisson(pcd, depth=depth)
    mesh.remove_vertices_by_mask(np.asarray(dens) < np.quantile(np.asarray(dens), 0.05))
    o3d.io.write_triangle_mesh(str(out_ply), mesh)
    return {"vertices": len(mesh.vertices), "faces": len(mesh.triangles)}


def export_glb(
    textured_obj: Path | None, mesh_ply: Path, glb: Path, obj_v_down: bool = True
) -> str:
    """Textured OBJ -> GLB when available, else the untextured PLY. Returns what was exported.

    OpenMVS writes texture coordinates with v measured from the TOP of the atlas (measured, see
    recon/render.py). trimesh assumes OBJ v points up and flips it for glTF, so for OpenMVS output
    v is flipped first: the GLB then samples the atlas exactly as the OBJ does
    (tests/test_render.py::test_glb_export_matches_openmvs_obj)."""
    import trimesh

    src = textured_obj if textured_obj is not None and textured_obj.exists() else mesh_ply
    scene = trimesh.load(str(src), process=False, force="scene")
    if src == textured_obj and obj_v_down:
        for g in scene.geometry.values():
            vis = getattr(g, "visual", None)
            if isinstance(vis, trimesh.visual.TextureVisuals) and vis.uv is not None:
                uv = np.asarray(vis.uv, float).copy()
                uv[:, 1] = 1.0 - uv[:, 1]
                vis.uv = uv
    scene.export(str(glb))
    return "textured" if src == textured_obj else "untextured"


def run_dense_mesh_texture(
    scene: SparseScene,
    out: Path,
    p: DenseParams | None = None,
    image_names: list[str] | None = None,
) -> dict:
    """Returns {"outputs": {...}, "timings_s": {...}, "stats": {...}, "backend": ...}."""
    p = p or DenseParams()
    out.mkdir(parents=True, exist_ok=True)
    t: dict[str, float] = {}
    outputs: dict[str, str] = {}
    stats: dict = {"params": asdict(p)}
    (out / "dense").mkdir(exist_ok=True)
    (out / "mesh" / "textured").mkdir(parents=True, exist_ok=True)

    if not openmvs.available():
        raise openmvs.OpenMVSError(
            "docker image room-openmvs:2.4.0 not available: dense reconstruction needs it "
            "(docker build -t room-openmvs:2.4.0 docker/openmvs)"
        )
    t0 = time.perf_counter()
    export_for_openmvs(scene, out, image_names)
    t["interface_colmap"] = time.perf_counter() - t0

    imgs = scene.images_dir
    common = ["--max-threads", str(p.max_threads)]
    r = openmvs.run_tool(
        "DensifyPointCloud",
        [
            "-i",
            "scene.mvs",
            "-o",
            "dense/scene_dense.mvs",
            "--resolution-level",
            str(p.resolution_level),
            "--min-resolution",
            str(p.min_resolution),
            "--number-views",
            str(p.number_views),
            "--remove-dmaps",
            "1",
            *common,
        ],
        out,
        imgs,
        p.timeout_s,
        p.memory_gb,
    )
    t["densify"] = r.seconds
    dense_ply = out / "dense" / "scene_dense.ply"
    outputs["dense_ply"] = str(dense_ply.relative_to(out))

    r = openmvs.run_tool(
        "ReconstructMesh",
        [
            "-i",
            "dense/scene_dense.mvs",
            "-o",
            "mesh/mesh.mvs",
            "--export-type",
            "ply",
            "--decimate",
            str(p.decimate),
            *common,
        ],
        out,
        imgs,
        p.timeout_s,
        p.memory_gb,
    )
    t["reconstruct_mesh"] = r.seconds
    mesh_ply = out / "mesh" / "mesh.ply"
    texture_mesh_src = mesh_ply
    if p.refine:
        r = openmvs.run_tool(
            "RefineMesh",
            [
                "-i",
                "mesh/mesh.mvs",
                "-m",
                "mesh/mesh.ply",
                "-o",
                "mesh/mesh_refined.mvs",
                "--export-type",
                "ply",
                "--resolution-level",
                str(p.resolution_level),
                "--min-resolution",
                str(p.min_resolution),
                *common,
            ],
            out,
            imgs,
            p.timeout_s,
            p.memory_gb,
        )
        t["refine_mesh"] = r.seconds
        texture_mesh_src = out / "mesh" / "mesh_refined.ply"
        outputs["mesh_refined_ply"] = str(texture_mesh_src.relative_to(out))
    outputs["mesh_ply"] = str(mesh_ply.relative_to(out))

    obj = None
    if p.texture:
        r = openmvs.run_tool(
            "TextureMesh",
            [
                "-i",
                "dense/scene_dense.mvs",
                "-m",
                str(texture_mesh_src.relative_to(out)),
                "-o",
                "mesh/textured/mesh.mvs",
                "--export-type",
                "obj",
                "--resolution-level",
                str(p.resolution_level),
                "--min-resolution",
                str(p.min_resolution),
                *common,
            ],
            out,
            imgs,
            p.timeout_s,
            p.memory_gb,
        )
        t["texture_mesh"] = r.seconds
        obj = _newest(out / "mesh" / "textured", "*.obj")
        outputs["textured_obj"] = str(obj.relative_to(out))
    t0 = time.perf_counter()
    stats["glb"] = export_glb(obj, texture_mesh_src, out / "mesh" / "mesh.glb")
    t["glb_export"] = time.perf_counter() - t0
    outputs["mesh_glb"] = "mesh/mesh.glb"

    m = o3d.io.read_triangle_mesh(str(mesh_ply))
    d = o3d.io.read_point_cloud(str(dense_ply))
    stats |= {
        "dense_points": len(d.points),
        "mesh_vertices": len(m.vertices),
        "mesh_faces": len(m.triangles),
    }
    return {"outputs": outputs, "timings_s": t, "stats": stats, "backend": "openmvs"}
