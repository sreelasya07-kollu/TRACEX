"""Synthetic SpatialMap with exact ground truth (box or L-shaped room) for tests of everything
downstream of Part 3: exports, QA gate, router. No COLMAP and no datasets: the cloud is the
TSDF fusion of ray-cast depth (eval/synthetic_room.py), the poses are the synthetic cameras, the
sparse model files are placeholders (their bytes are not parsed by the consumers tested here)."""

from __future__ import annotations

import json
from pathlib import Path

import cv2
import numpy as np
import open3d as o3d

from eval.synthetic_room import camera_model, cameras, fused_cloud
from formats.models import Intrinsics, Priors, StageMeta
from spatial.room.run import run_room
from spatial.spatial_map import write_spatial_map


def _meta(stage: str, metrics: dict | None = None) -> str:
    return StageMeta(
        stage=stage,
        status="ok",
        started="2026-11-14T00:00:00+00:00",
        ended="2026-11-14T00:00:01+00:00",
        wall_time_s=1.0,
        device="cpu",
        peak_ram_mb=100.0,
        peak_vram_mb=None,
        versions={},
        params={},
        inputs={},
        outputs={},
        metrics=metrics or {},
    ).model_dump_json()


def build_synthetic_spatial_map(
    root: Path,
    layout: str = "box",
    seed: int = 0,
    keep_camera=None,
    mean_reproj_px: float = 0.5,
) -> Path:
    """Writes `<root>/spatial_map` and returns it. `keep_camera(T_wc) -> bool` filters which
    synthetic cameras are *registered* (the cloud is always fused from all of them), which lets
    tests create a coverage gap without changing the geometry."""
    root.mkdir(parents=True, exist_ok=True)
    ws, p2, p3, p5 = root / "ws", root / "phase2", root / "phase3", root / "phase5"
    for d in (ws / "sparse" / "0", ws / "images", p2, p3 / "depth_aligned", p3 / "depth_valid"):
        d.mkdir(parents=True, exist_ok=True)
    (p3 / "fusion").mkdir(parents=True, exist_ok=True)
    K, W, H = camera_model()
    poses = cameras(seed, layout)
    if keep_camera is not None:
        poses = [T for T in poses if keep_camera(T)]
    ids = [f"{i:06d}" for i in range(len(poses))]
    T_list = [T.tolist() for T in poses]
    pri = Priors(
        width=W,
        height=H,
        intrinsics=Intrinsics(fx=K[0, 0], fy=K[1, 1], cx=K[0, 2], cy=K[1, 2]),
        frames=[
            {"id": i, "timestamp_ns": k, "image": f"images/{i}.jpg", "T_world_camera": T}
            for k, (i, T) in enumerate(zip(ids, T_list, strict=True))
        ],
        pose_source="dataset_gt_noisy",
        gravity_world=[0, 0, -1],
        gravity_source="dataset_axis",
        depth_source="rendered",
    )
    (ws / "priors.json").write_text(pri.model_dump_json())
    (ws / "alignment.json").write_text(
        json.dumps({"metric": True, "gravity_aligned": True, "prior_residual_p95_m": 0.01})
    )
    (ws / "poses_refined.json").write_text(json.dumps(dict(zip(ids, T_list, strict=True))))
    (ws / "workspace_meta.json").write_text(json.dumps({"dataset": "synthetic", "scene": layout}))
    for n in ("cameras.bin", "images.bin", "points3D.bin"):
        (ws / "sparse" / "0" / n).write_bytes(n.encode())
    (p2 / "meta.json").write_text(
        _meta(
            "phase2_refine",
            {
                "registered": len(ids),
                "n_frames": len(ids),
                "reg_ratio": 1.0,
                "mean_reproj_px": mean_reproj_px,
            },
        )
    )
    for i in ids:
        cv2.imwrite(str(ws / "images" / f"{i}.jpg"), np.zeros((H, W, 3), np.uint8))
        cv2.imwrite(str(p3 / "depth_aligned" / f"{i}.png"), np.full((H, W), 2000, np.uint16))
        cv2.imwrite(str(p3 / "depth_valid" / f"{i}.png"), np.full((H, W), 255, np.uint8))
    (p3 / "depth_alignment.json").write_text("{}")
    o3d.io.write_point_cloud(
        str(p3 / "fusion" / "cloud.ply"), fused_cloud(seed=seed, layout=layout)
    )
    o3d.io.write_triangle_mesh(
        str(p3 / "fusion" / "tsdf_mesh.ply"), o3d.geometry.TriangleMesh.create_box()
    )
    (p3 / "meta.json").write_text(
        _meta(
            "phase3",
            {
                "frame_ids": ids,
                "width": W,
                "height": H,
                "valid_fraction": 0.9,
                "depth_source": "sensor",
                "n_points": 100000,
            },
        )
    )
    run_room(
        p3 / "fusion" / "cloud.ply", p5, scale_source="metric_priors", alignment={"metric": True}
    )
    return write_spatial_map(ws, p2, p3, p5, root / "spatial_map", link=False)
