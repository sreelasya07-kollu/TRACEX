"""Pydantic models for every on-disk JSON format (see docs/INTERFACES.md)."""

from __future__ import annotations

from typing import Literal

import numpy as np
from pydantic import BaseModel, ConfigDict, Field, field_validator

FORMAT_VERSION = "0.1.0"

Mat4 = list[list[float]]


def _check_rigid(m: Mat4) -> Mat4:
    a = np.asarray(m, dtype=float)
    if a.shape != (4, 4):
        raise ValueError("pose must be 4x4")
    if not np.allclose(a[3], [0, 0, 0, 1], atol=1e-9):
        raise ValueError("last row must be [0,0,0,1]")
    R = a[:3, :3]
    if not np.allclose(R @ R.T, np.eye(3), atol=1e-5) or np.linalg.det(R) < 0:
        raise ValueError("rotation block is not a proper rotation")
    return m


class Strict(BaseModel):
    model_config = ConfigDict(extra="forbid")


class Intrinsics(Strict):
    fx: float = Field(gt=0, description="focal length x [px]")
    fy: float = Field(gt=0, description="focal length y [px]")
    cx: float = Field(description="principal point x [px]")
    cy: float = Field(description="principal point y [px]")

    def K(self) -> np.ndarray:
        return np.array([[self.fx, 0, self.cx], [0, self.fy, self.cy], [0, 0, 1.0]])


class FramePrior(Strict):
    id: str = Field(pattern=r"^\d{6}$")
    timestamp_ns: int
    image: str
    depth: str | None = None
    depth_conf: str | None = None
    T_world_camera: Mat4 | None = None

    @field_validator("T_world_camera")
    @classmethod
    def _rigid(cls, v):
        return None if v is None else _check_rigid(v)


class PoseNoise(Strict):
    preset: str
    seed: int
    trans_drift_per_m: float = Field(
        description="random-walk translation drift [m per m travelled]"
    )
    yaw_drift_deg_per_m: float = Field(description="random-walk yaw drift [deg per m travelled]")
    jitter_trans_m: float
    jitter_rot_deg: float


class Priors(Strict):
    version: str = FORMAT_VERSION
    width: int = Field(gt=0)
    height: int = Field(gt=0)
    intrinsics: Intrinsics
    frames: list[FramePrior]
    pose_source: Literal["arkit", "arcore", "dataset_gt", "dataset_gt_noisy", "none"]
    pose_noise: PoseNoise | None = None
    gravity_world: list[float] | None = Field(None, description="unit vector pointing down")
    gravity_source: Literal["arkit", "imu_accel", "dataset_axis", "none"]
    depth_scale_m: float = 0.001
    depth_source: Literal["lidar", "kinect", "rendered", "none"]

    @field_validator("gravity_world")
    @classmethod
    def _unit(cls, v):
        if v is not None and abs(np.linalg.norm(v) - 1) > 1e-6:
            raise ValueError("gravity_world must be a unit vector")
        return v


class WorkspaceMeta(Strict):
    version: str = FORMAT_VERSION
    dataset: str
    scene: str
    source_path: str
    frame_ids_source: list[int] = Field(description="indices into the dataset's own frame list")
    stride: int
    max_frames: int | None
    downscale: int
    distortion_removed: bool
    adapter_version: str
    notes: list[str] = []


class StageMeta(Strict):
    stage: str
    status: Literal["ok", "failed", "partial"]
    started: str
    ended: str
    wall_time_s: float
    device: str
    peak_ram_mb: float
    peak_vram_mb: float | None
    versions: dict[str, str]
    params: dict
    inputs: dict[str, str]
    outputs: dict[str, str]
    metrics: dict
    notes: list[str] = []


class FrameAlignment(Strict):
    source: Literal["sensor", "mono", "mvs"]
    scale: float | None
    shift: float | None
    scale_smoothed: float | None = None
    inliers: int = 0
    n_points: int = 0
    residual_m: float | None = None
    status: Literal["ok", "rejected_few_points", "rejected_residual", "no_alignment"]


class DepthModelInfo(Strict):
    name: str
    hf_id: str | None
    revision: str | None
    kind: Literal["metric", "relative", "inverse_relative", "sensor", "mvs"]
    licence: str


class DepthAlignment(Strict):
    version: str = FORMAT_VERSION
    model: DepthModelInfo
    global_scale: float | None = None
    frames: dict[str, FrameAlignment]


# ---- Phase 5: planes + room ------------------------------------------------------------


class Plane(Strict):
    id: int
    label: Literal["floor", "ceiling", "wall", "other"]
    normal: list[float] = Field(description="unit normal, world frame (+Z up)")
    d: float = Field(description="plane: normal . x + d = 0 (metres)")
    inlier_count: int = Field(ge=0)
    area_m2: float = Field(ge=0, description="area of the inlier footprint (convex hull)")
    rms_m: float = Field(ge=0, description="RMS point-to-plane distance of inliers")
    polygon: list[list[float]] = Field(
        description="convex hull of inliers projected onto the plane, 3D world points (metres)"
    )

    @field_validator("normal")
    @classmethod
    def _unit(cls, v):
        if len(v) != 3 or abs(np.linalg.norm(v) - 1) > 1e-6:
            raise ValueError("normal must be a unit 3-vector")
        return v


class PlanesFile(Strict):
    version: str = FORMAT_VERSION
    planes: list[Plane]
    n_points: int
    n_assigned: int
    params: dict


class RoomModel(Strict):
    version: str = FORMAT_VERSION
    floor_polygon: list[list[float]] = Field(
        description="2D (x, y) polygon in world frame, counter-clockwise, metres"
    )
    floor_polygon_source: Literal["wall_intersections", "footprint_contour", "floor_hull", "none"]
    floor_area_m2: float | None
    floor_z: float | None
    ceiling_z: float | None
    ceiling_height_m: float | None
    floor_id: int | None
    ceiling_id: int | None
    wall_ids: list[int]
    floor_tilt_deg: float | None = Field(
        None, description="angle between the floor normal and +Z (residual gravity error)"
    )
    R_floor_up: list[list[float]] | None = Field(
        None, description="rotation taking the floor normal to +Z; NOT applied to the data"
    )
    bbox_min: list[float]
    bbox_max: list[float]
    gravity_world: list[float] = Field(description="unit vector pointing down")
    scale_source: Literal["metric_priors", "sensor_depth", "synthetic", "unknown"]
    scale_confidence: float | None = Field(
        None,
        ge=0,
        le=1,
        description="HEURISTIC trust in the metric scale (not a calibrated probability); see "
        "spatial.room.run.scale_confidence",
    )
    notes: list[str] = []


# ---- Phase 6: SpatialMap (Part 3 -> Part 4 hand-over) -----------------------------------


class StageSummary(Strict):
    status: Literal["ok", "failed", "partial"]
    wall_time_s: float
    peak_ram_mb: float
    device: str


class SpatialMapManifest(Strict):
    format: Literal["room.spatial_map"] = "room.spatial_map"
    version: str = FORMAT_VERSION
    created: str
    source: dict = Field(description="dataset / scene / source_path from workspace_meta.json")
    frame_ids: list[str]
    files: dict[str, str] = Field(description="relative path -> sha256 of every file but this one")
    stages: dict[str, StageSummary]
    params: dict
    versions: dict[str, str]
    conventions: dict[str, str]


class CamerasFile(Strict):
    version: str = FORMAT_VERSION
    width: int = Field(gt=0)
    height: int = Field(gt=0)
    intrinsics: Intrinsics
    depth_width: int = Field(gt=0)
    depth_height: int = Field(gt=0)
    depth_intrinsics: Intrinsics
    pose_source: str
    metric: bool
    gravity_aligned: bool
    frames: dict[str, Mat4] = Field(description="frame id -> T_world_camera (refined)")

    @field_validator("frames")
    @classmethod
    def _rigid_frames(cls, v):
        for m in v.values():
            _check_rigid(m)
        return v


class SpatialQuality(Strict):
    version: str = FORMAT_VERSION
    n_frames: int
    registered: int
    reg_ratio: float
    mean_reproj_px: float | None
    scale_source: str
    scale_confidence: float | None
    depth_source: str
    depth_valid_fraction: float
    n_points: int
    n_planes: int
    n_walls: int
    wall_floor_max_dev_deg: float | None
    floor_tilt_deg: float | None
    ceiling_height_m: float | None
    floor_area_m2: float | None
    floor_polygon_source: str
    warnings: list[str]


# ---- Phase 10: Reconstruction (Part 4 output) --------------------------------------------


class OutputEntry(Strict):
    """One Part 4 product. Mesh and splat are separate entries with separate metrics: neither
    is derived from the other."""

    status: Literal["ok", "skipped", "failed"]
    reason: str | None = Field(None, description="why skipped / failed")
    files: list[str] = Field(default=[], description="paths relative to the reconstruction root")
    metrics: dict = {}


class ReconstructionManifest(Strict):
    format: Literal["room.reconstruction"] = "room.reconstruction"
    version: str = FORMAT_VERSION
    created: str
    spatial_map: dict = Field(description="path, manifest_sha256 and quality warnings of the input")
    outputs: dict[str, OutputEntry] = Field(description="sparse, dense, mesh, splat")
    files: dict[str, str] = Field(description="relative path -> sha256 of every file but this one")
    timings_s: dict[str, float]
    peak_ram_mb: float
    params: dict
    versions: dict[str, str]


# ---- QA gate / Job Report (HANDOFF §3 stage 9; spec D-ScanQA) ----------------------------


class GateResult(Strict):
    name: str
    category: Literal["capture", "output"] = Field(
        description="capture: fixable by rescanning; output: a processing-quality result"
    )
    status: Literal["pass", "warn", "fail", "skipped"]
    value: float | str | None = None
    threshold: str
    message: str


class RescanRegion(Strict):
    """A surface patch the scan did not observe well enough, anchored in the world frame."""

    surface: Literal["wall", "floor", "ceiling"]
    wall_index: int | None = Field(None, description="index of the floor-polygon edge")
    reason: Literal["unseen", "few_views", "narrow_baseline"]
    corners: list[list[float]] = Field(description="4 world-frame corners (metres)")
    area_m2: float
    priority: float = Field(ge=0, le=1)
    instruction: str


class JobReport(Strict):
    version: str = FORMAT_VERSION
    verdict: Literal["pass", "pass_with_warnings", "needs_rescan"]
    gates: list[GateResult]
    coverage: dict[str, float] = Field(description="observed fraction per surface type")
    rescan: list[RescanRegion]
    inputs: dict
    notes: list[str] = []
