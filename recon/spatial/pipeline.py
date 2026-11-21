"""One command for Part 3: ingest -> refine -> depth+fusion -> planes/room -> SpatialMap."""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path

from spatial.fusion.tsdf import TSDFParams
from spatial.io.ingest import ingest
from spatial.io.keyframes import KeyframeParams
from spatial.phase3 import Phase3Config, run_phase3
from spatial.room.planes import PlaneParams
from spatial.room.run import run_room
from spatial.sfm.refine import refine
from spatial.spatial_map import write_spatial_map


def run_pipeline(
    dataset: str,
    src: Path,
    run: Path,
    *,
    max_frames: int | None = None,
    stride: int = 1,
    downscale: int = 1,
    prior: str = "gt_noisy",
    noise_preset: str = "vio_small",
    seed: int = 0,
    max_image_size: int = 1024,
    depth_scale: bool = False,
    phase3: Phase3Config | None = None,
    voxel: float = 0.02,
    keyframes: KeyframeParams | None = None,
) -> Path:
    """Returns the spatial_map directory (<run>/spatial_map). Needs gravity (priors) for a
    meaningful room model; without it `room.json` labels are flagged in quality.json."""
    ws = ingest(
        dataset,
        src,
        run,
        max_frames=max_frames,
        stride=stride,
        downscale=downscale,
        prior=prior,
        noise_preset=noise_preset,
        seed=seed,
        keyframes=keyframes,
    )
    refine(ws, run, max_image_size=max_image_size, depth_scale=depth_scale)
    cfg = phase3 or Phase3Config(downscale=2, tsdf_p=TSDFParams(voxel=voxel))
    cfg = replace(cfg, seed=seed)
    run_phase3(ws, run / "phase3", cfg)
    alignment = json.loads((ws / "alignment.json").read_text())
    if alignment.get("scale_source") == "sensor_depth":
        scale_source = "sensor_depth"
    else:
        scale_source = "metric_priors" if prior != "none" else "unknown"
    run_room(
        run / "phase3" / "fusion" / "cloud.ply",
        run / "phase5",
        PlaneParams(seed=seed),
        scale_source,
        alignment,
    )
    return write_spatial_map(
        ws, run / "phase2", run / "phase3", run / "phase5", run / "spatial_map"
    )
