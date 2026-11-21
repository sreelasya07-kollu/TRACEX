"""Phase 6: assemble the SpatialMap (docs/INTERFACES.md §6) from the Phase 2/3/5 outputs.

Large files are hard-linked when possible (same filesystem) and copied otherwise, so a
SpatialMap costs almost no extra disk. The manifest lists the sha256 of every other file."""

from __future__ import annotations

import json
import os
import shutil
from datetime import UTC, datetime
from pathlib import Path

from formats.models import (
    FORMAT_VERSION,
    CamerasFile,
    Intrinsics,
    PlanesFile,
    Priors,
    RoomModel,
    SpatialMapManifest,
    SpatialQuality,
    StageMeta,
    StageSummary,
)
from formats.spatial_map import MANIFEST
from spatial.phase3 import _scaled_K
from spatial.room.run import wall_floor_angles_deg
from spatial.stage import sha256_file, versions

CONVENTIONS = {
    "units": "metres, radians",
    "camera_frame": "OpenCV: x right, y down, z forward",
    "world_frame": "right-handed, +Z up (gravity-aligned when cameras.json gravity_aligned)",
    "pose": "T_world_camera (camera to world), 4x4 row-major",
    "depth_png": "uint16 millimetres, 0 = invalid; depth_valid/*.png is the final mask (255)",
    "colmap_model": "sparse/ stores cam_from_world as COLMAP does",
    "images": "images/<frame>.jpg, undistorted, at cameras.json width x height",
}


def _put(src: Path, dst: Path, link: bool) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if link:
        try:
            os.link(src, dst)
            return
        except OSError:
            pass
    shutil.copy2(src, dst)


def _stage(meta_path: Path) -> StageSummary:
    m = StageMeta.model_validate_json(meta_path.read_text())
    return StageSummary(
        status=m.status, wall_time_s=m.wall_time_s, peak_ram_mb=m.peak_ram_mb, device=m.device
    )


def quality_report(
    cams: CamerasFile,
    align: dict,
    p3_metrics: dict,
    planes: PlanesFile,
    room: RoomModel,
    reg: dict,
) -> SpatialQuality:
    walls = [q for q in planes.planes if q.label == "wall"]
    dev = wall_floor_angles_deg(planes.planes)
    w: list[str] = []
    if reg["reg_ratio"] < 0.95:
        w.append(f"only {reg['reg_ratio']:.0%} of frames registered")
    if reg["mean_reproj_px"] is not None and reg["mean_reproj_px"] > 1.0:
        w.append(f"mean reprojection error {reg['mean_reproj_px']:.2f} px > 1.0")
    sizes = reg.get("model_sizes") or []
    if len(sizes) > 1:
        w.append(
            f"SfM split into {len(sizes)} models (sizes {', '.join(map(str, sizes))}); "
            f"kept the largest; largest model has {max(sizes)}/{reg['n_frames']} frames"
        )
    if not cams.gravity_aligned:
        w.append("world frame is not gravity-aligned: room labels are unreliable")
    if not cams.metric:
        w.append("model is not metric")
    if room.floor_id is None:
        w.append("no floor plane found")
    if room.ceiling_id is None:
        w.append("no ceiling plane found")
    if room.floor_polygon_source in ("floor_hull", "none"):
        w.append(f"floor outline fell back to {room.floor_polygon_source}")
    if len(walls) < 3:
        w.append(f"only {len(walls)} wall plane(s) detected")
    if dev and max(dev) > 3.0:
        w.append(f"a wall deviates {max(dev):.1f} deg from perpendicular to the floor")
    if room.floor_tilt_deg is not None and room.floor_tilt_deg > 2.0:
        w.append(
            f"floor normal is {room.floor_tilt_deg:.1f} deg from +Z (gravity prior error); "
            "room.json R_floor_up holds the correction"
        )
    if room.scale_confidence is not None and room.scale_confidence < 0.5:
        w.append(f"low scale confidence ({room.scale_confidence:.2f})")
    if p3_metrics["valid_fraction"] < 0.3:
        w.append(f"only {p3_metrics['valid_fraction']:.0%} of depth pixels survived filtering")
    return SpatialQuality(
        n_frames=reg["n_frames"],
        registered=reg["registered"],
        reg_ratio=reg["reg_ratio"],
        mean_reproj_px=reg["mean_reproj_px"],
        scale_source=room.scale_source,
        scale_confidence=room.scale_confidence,
        depth_source=p3_metrics["depth_source"],
        depth_valid_fraction=p3_metrics["valid_fraction"],
        n_points=p3_metrics["n_points"],
        n_planes=len(planes.planes),
        n_walls=len(walls),
        wall_floor_max_dev_deg=max(dev) if dev else None,
        floor_tilt_deg=room.floor_tilt_deg,
        ceiling_height_m=room.ceiling_height_m,
        floor_area_m2=room.floor_area_m2,
        floor_polygon_source=room.floor_polygon_source,
        warnings=w,
    )


def write_spatial_map(
    ws: Path,
    phase2_dir: Path,
    phase3_dir: Path,
    phase5_dir: Path,
    out: Path,
    link: bool = True,
) -> Path:
    """`out` is the spatial_map directory (created; must not exist)."""
    if out.exists():
        raise FileExistsError(out)
    pri = Priors.model_validate_json((ws / "priors.json").read_text())
    align = json.loads((ws / "alignment.json").read_text())
    poses = json.loads((ws / "poses_refined.json").read_text())
    wmeta = json.loads((ws / "workspace_meta.json").read_text())
    p3 = StageMeta.model_validate_json((phase3_dir / "meta.json").read_text())
    ids = p3.metrics["frame_ids"]
    D = pri.width // p3.metrics["width"]
    K = pri.intrinsics.K()
    Kd = _scaled_K(K, D)
    cams = CamerasFile(
        width=pri.width,
        height=pri.height,
        intrinsics=pri.intrinsics,
        depth_width=p3.metrics["width"],
        depth_height=p3.metrics["height"],
        depth_intrinsics=Intrinsics(fx=Kd[0, 0], fy=Kd[1, 1], cx=Kd[0, 2], cy=Kd[1, 2]),
        pose_source=pri.pose_source,
        metric=bool(align.get("metric")),
        gravity_aligned=bool(align.get("gravity_aligned")),
        frames={k: poses[k] for k in sorted(poses)},
    )
    pri_frame_image = {f.id: f.image for f in pri.frames}
    planes = PlanesFile.model_validate_json((phase5_dir / "planes.json").read_text())
    room = RoomModel.model_validate_json((phase5_dir / "room.json").read_text())
    p2 = StageMeta.model_validate_json((phase2_dir / "meta.json").read_text())
    reg = {
        k: p2.metrics.get(k)
        for k in ("registered", "n_frames", "reg_ratio", "mean_reproj_px", "model_sizes")
    }
    quality = quality_report(cams, align, p3.metrics, planes, room, reg)

    out.mkdir(parents=True)
    (out / "cameras").mkdir()
    (out / "cameras" / "cameras.json").write_text(cams.model_dump_json(indent=1))
    for n in ("cameras.bin", "images.bin", "points3D.bin"):
        _put(ws / "sparse" / "0" / n, out / "sparse" / n, link)
    for i in cams.frames:  # all registered frames: Part 4 (texture, splats) needs the images
        _put(ws / pri_frame_image[i], out / "images" / f"{i}.jpg", link)
    for i in ids:
        _put(phase3_dir / "depth_aligned" / f"{i}.png", out / "depth" / f"{i}.png", link)
        _put(phase3_dir / "depth_valid" / f"{i}.png", out / "depth_valid" / f"{i}.png", link)
    _put(phase3_dir / "depth_alignment.json", out / "depth" / "depth_alignment.json", link)
    _put(phase3_dir / "fusion" / "cloud.ply", out / "fused_cloud.ply", link)
    _put(phase3_dir / "fusion" / "tsdf_mesh.ply", out / "tsdf_mesh.ply", link)
    _put(phase5_dir / "planes.json", out / "planes.json", link)
    _put(phase5_dir / "room.json", out / "room.json", link)
    (out / "quality.json").write_text(quality.model_dump_json(indent=1))

    files = {
        p.relative_to(out).as_posix(): sha256_file(p)
        for p in sorted(out.rglob("*"))
        if p.is_file()
    }
    man = SpatialMapManifest(
        version=FORMAT_VERSION,
        created=datetime.now(UTC).isoformat(),
        source={k: wmeta.get(k) for k in ("dataset", "scene", "source_path")},
        frame_ids=ids,
        files=files,
        stages={
            "phase2_refine": _stage(phase2_dir / "meta.json"),
            "phase3_depth_fusion": _stage(phase3_dir / "meta.json"),
            "phase5_room": _stage(phase5_dir / "meta.json"),
        },
        params={
            "phase2": p2.params,
            "phase3": p3.params,
            "phase5": json.loads((phase5_dir / "meta.json").read_text())["params"],
        },
        versions=versions(),
        conventions=CONVENTIONS,
    )
    (out / MANIFEST).write_text(man.model_dump_json(indent=1))
    return out


__all__ = ["write_spatial_map", "quality_report"]
