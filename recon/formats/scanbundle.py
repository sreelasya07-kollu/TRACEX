"""Scan Bundle v0.1: the phone-to-backend capture package. PROPOSAL from the Part 3/4 side: the
CaptureKit spec does not exist yet, so this is what the backend needs and checks. Documented in
docs/SCAN_BUNDLE.md; JSON Schema in spec/scanbundle/manifest.schema.json.

Layout:  manifest.json  images/<id>.jpg  [depth/<id>.png (uint16 mm, same size as the image)]
Pure pydantic + numpy + PIL (image headers only), so the mobile team can reuse the validator."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Literal

import numpy as np
from PIL import Image
from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator

from formats.models import Mat4, _check_rigid

SCHEMA_VERSION = "0.1.0"
MIN_FRAMES = 20


class _S(BaseModel):
    model_config = ConfigDict(extra="forbid")


class Device(_S):
    model: str
    os: str
    app_version: str


class Distortion(_S):
    model: Literal["opencv"] = "opencv"
    coeffs: list[float] = Field(description="k1, k2, p1, p2[, k3]", min_length=4, max_length=5)


class BundleIntrinsics(_S):
    fx: float = Field(gt=0)
    fy: float = Field(gt=0)
    cx: float
    cy: float
    width: int = Field(gt=0)
    height: int = Field(gt=0)
    distortion: Distortion | None = None


class Coordinates(_S):
    world_up: Literal["+Y", "+Z"] = Field(description="up axis of the pose world frame")
    camera: Literal["opencv", "arkit", "opengl"] = Field(
        description="camera axes of T_world_camera: opencv x right y down z fwd; "
        "arkit/opengl x right y up z backward"
    )


class Frame(_S):
    id: str = Field(pattern=r"^[A-Za-z0-9_\-]{1,32}$")
    timestamp_ns: int
    image: str
    depth: str | None = None
    depth_confidence: str | None = None
    T_world_camera: Mat4 | None = None
    tracking_state: Literal["normal", "limited", "not_available"] = "normal"
    exposure_s: float | None = None
    iso: float | None = None

    @field_validator("T_world_camera")
    @classmethod
    def _rigid(cls, v):
        return None if v is None else _check_rigid(v)


class Consent(_S):
    scan_processing: bool
    training_use: bool = False
    faces_blurred: bool = False


class ScanBundleManifest(_S):
    schema_version: str
    capture_id: str
    created: str
    device: Device
    tier: Literal["A", "B", "C"]
    units: Literal["metres"] = "metres"
    coordinates: Coordinates
    gravity_world: list[float] | None = Field(None, description="unit vector pointing DOWN")
    gravity_source: Literal["arkit", "imu_accel", "none"] = "none"
    pose_source: Literal["arkit", "arcore", "none"]
    intrinsics: BundleIntrinsics
    frames: list[Frame]
    consent: Consent
    quality_report: dict | None = None
    files: dict[str, str] = Field(description="relative path -> sha256 of every file but manifest")


@dataclass
class Issue:
    level: Literal["error", "warning"]
    code: str
    message: str
    frame_id: str | None = None

    def __str__(self) -> str:
        loc = f" [frame {self.frame_id}]" if self.frame_id else ""
        return f"{self.level.upper()} {self.code}{loc}: {self.message}"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def _rot_deg(Ra: np.ndarray, Rb: np.ndarray) -> float:
    c = (np.trace(Ra.T @ Rb) - 1) / 2
    return float(np.degrees(np.arccos(np.clip(c, -1, 1))))


def validate_bundle(
    root: Path,
    *,
    max_speed_mps: float = 8.0,
    max_step_deg: float = 90.0,
    gap_warn_s: float = 1.0,
    check_checksums: bool = True,
) -> tuple[list[Issue], ScanBundleManifest | None]:
    """All checks; returns (issues, manifest or None when the manifest itself is unusable)."""
    root = Path(root)
    mp = root / "manifest.json"
    if not mp.is_file():
        return [Issue("error", "E_NO_MANIFEST", f"{mp} not found")], None
    try:
        man = ScanBundleManifest.model_validate_json(mp.read_text())
    except ValidationError as e:
        errs = e.errors()
        return [
            Issue("error", "E_SCHEMA", f"{'.'.join(map(str, x['loc']))}: {x['msg']}")
            for x in errs[:10]
        ], None
    except ValueError as e:  # malformed JSON
        return [Issue("error", "E_SCHEMA", f"manifest is not valid JSON: {e}")], None
    I: list[Issue] = []  # noqa: E741

    def err(code, msg, fid=None):
        I.append(Issue("error", code, msg, fid))

    def warn(code, msg, fid=None):
        I.append(Issue("warning", code, msg, fid))

    if man.schema_version.split(".")[0] != SCHEMA_VERSION.split(".")[0]:
        err("E_VERSION", f"unsupported schema_version {man.schema_version} (supported 0.x)")
    try:
        datetime.fromisoformat(man.created)
    except ValueError:
        err("E_SCHEMA", f"created is not an ISO 8601 timestamp: {man.created!r}")
    if not man.consent.scan_processing:
        err("E_CONSENT", "consent.scan_processing is false: the bundle must not be processed")
    if not man.consent.faces_blurred:
        warn("W_PRIVACY", "faces_blurred is false: run the privacy pass before human review")

    # files + checksums
    listed = set(man.files)
    for rel, digest in man.files.items():
        p = root / rel
        if not p.is_file():
            err("E_FILE_MISSING", rel)
        elif check_checksums and sha256(p) != digest:
            err("E_CHECKSUM", f"{rel} does not match its sha256")
    on_disk = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()} - {"manifest.json"}
    for rel in sorted(on_disk - listed):
        warn("W_UNLISTED_FILE", f"{rel} is not in manifest.files (ignored)")
    for f in man.frames:
        for rel in (f.image, f.depth, f.depth_confidence):
            if rel and rel not in listed:
                err(
                    "E_FILE_NOT_LISTED", f"{rel} is referenced by a frame but has no checksum", f.id
                )

    ids = [f.id for f in man.frames]
    if len(set(ids)) != len(ids):
        err("E_DUP_ID", "frame ids are not unique")
    if len(man.frames) < MIN_FRAMES:
        err("E_FRAME_COUNT", f"{len(man.frames)} frames; at least {MIN_FRAMES} are required")

    # intrinsics + image/depth sizes
    K = man.intrinsics
    if not (0.3 * K.width <= K.fx <= 3 * K.width and 0.3 * K.width <= K.fy <= 3 * K.width):
        warn("W_INTRINSICS", f"focal length fx={K.fx}, fy={K.fy} is unusual for {K.width} px width")
    for f in man.frames[:3] + man.frames[-1:]:  # header check on a sample keeps this fast
        for rel, what in ((f.image, "image"), (f.depth, "depth")):
            if rel and (root / rel).is_file():
                try:
                    with Image.open(root / rel) as im:
                        if im.size != (K.width, K.height):
                            err(
                                "E_SIZE",
                                f"{what} is {im.size}, intrinsics say "
                                f"{(K.width, K.height)} (depth must be aligned to the colour image)",
                                f.id,
                            )
                        if what == "depth" and im.mode not in ("I;16", "I", "L"):
                            err(
                                "E_DEPTH_FORMAT", f"depth PNG mode {im.mode}: expected 16-bit", f.id
                            )
                except OSError as e:
                    err("E_IMAGE_UNREADABLE", str(e), f.id)

    # time
    ts = np.array([f.timestamp_ns for f in man.frames], dtype=np.int64)
    if len(ts) > 1:
        dt = np.diff(ts) / 1e9
        if (dt <= 0).any():
            err(
                "E_TIMESTAMPS",
                f"timestamps are not strictly increasing ({int((dt <= 0).sum())} bad)",
            )
        for k in np.flatnonzero(dt > gap_warn_s):
            warn("W_FRAME_GAP", f"{dt[k]:.2f} s between frames", man.frames[k + 1].id)

    # gravity + poses by tier
    has_pose = [f.T_world_camera is not None for f in man.frames]
    if man.tier in ("A", "B"):
        if man.pose_source == "none":
            err("E_TIER", f"tier {man.tier} needs device poses (pose_source != none)")
        if sum(has_pose) < 0.9 * len(has_pose):
            err("E_POSES_MISSING", f"only {sum(has_pose)}/{len(has_pose)} frames have a pose")
        if man.gravity_world is None:
            err("E_GRAVITY", "tier A/B need gravity_world (room orientation depends on it)")
    else:
        if man.pose_source != "none" or any(has_pose):
            err("E_TIER", "tier C is RGB only: pose_source must be 'none' and poses absent")
    if man.tier == "A":
        n_depth = sum(f.depth is not None for f in man.frames)
        if n_depth < 0.9 * len(man.frames):
            err(
                "E_TIER",
                f"tier A needs depth on >= 90 % of frames, found {n_depth}/{len(man.frames)}",
            )
    if man.gravity_world is not None:
        g = np.asarray(man.gravity_world, float)
        if g.shape != (3,) or not np.isfinite(g).all() or abs(np.linalg.norm(g) - 1) > 1e-3:
            err("E_GRAVITY", f"gravity_world must be a unit 3-vector, got {man.gravity_world}")
        else:
            up = (
                np.array([0.0, 1.0, 0.0])
                if man.coordinates.world_up == "+Y"
                else np.array([0, 0, 1.0])
            )
            if float(g @ -up) < 0.98:
                warn("W_GRAVITY_AXIS", f"gravity is >11 deg from -{man.coordinates.world_up}")

    P = [
        (i, np.asarray(f.T_world_camera, float))
        for i, f in enumerate(man.frames)
        if f.T_world_camera is not None
    ]
    for i, T in P:
        if not np.isfinite(T).all():
            err("E_POSE_NAN", "pose contains NaN/Inf", man.frames[i].id)
    P = [(i, T) for i, T in P if np.isfinite(T).all()]
    for (i0, T0), (i1, T1) in zip(P, P[1:], strict=False):
        dts = (ts[i1] - ts[i0]) / 1e9
        if dts <= 0:
            continue
        d = float(np.linalg.norm(T1[:3, 3] - T0[:3, 3]))
        if d / dts > max_speed_mps:
            err(
                "E_POSE_TELEPORT",
                f"moved {d:.2f} m in {dts:.2f} s ({d / dts:.1f} m/s)",
                man.frames[i1].id,
            )
        if _rot_deg(T0[:3, :3], T1[:3, :3]) > max_step_deg:
            err(
                "E_POSE_JUMP",
                f"rotated {_rot_deg(T0[:3, :3], T1[:3, :3]):.0f} deg between frames",
                man.frames[i1].id,
            )
    limited = sum(f.tracking_state != "normal" for f in man.frames)
    if limited > 0.2 * len(man.frames):
        warn("W_TRACKING", f"{limited}/{len(man.frames)} frames have limited/no tracking")
    return I, man
