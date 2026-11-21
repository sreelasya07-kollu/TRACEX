"""Scan Bundle v0.1 -> SourceDataset (docs/SCAN_BUNDLE.md). The bundle is validated first; any
error raises BundleError with the issue codes. Device poses become *priors* (never ground truth):
the camera axes are converted to OpenCV, the world frame is left as captured (gravity_world is
given in it and Phase 2 aligns it to +Z)."""

from __future__ import annotations

from pathlib import Path

import numpy as np

from formats.scanbundle import validate_bundle
from spatial.io.adapters import SourceDataset, SourceFrame

# x right, y up, z backward (ARKit / OpenGL) -> x right, y down, z forward (OpenCV)
_FLIP = np.diag([1.0, -1.0, -1.0, 1.0])


class BundleError(ValueError):
    pass


def load_scanbundle(root: Path) -> SourceDataset:
    root = Path(root)
    issues, man = validate_bundle(root)
    errors = [i for i in issues if i.level == "error"]
    if errors or man is None:
        raise BundleError(f"{root}: " + "; ".join(str(e) for e in errors[:5]))
    K = man.intrinsics
    frames = []
    for k, f in enumerate(man.frames):
        T = None
        if f.T_world_camera is not None:
            T = np.asarray(f.T_world_camera, float)
            if man.coordinates.camera in ("arkit", "opengl"):
                T = T @ _FLIP
        frames.append(
            SourceFrame(
                index=k,
                timestamp_ns=f.timestamp_ns,
                rgb=root / f.image,
                depth=None if f.depth is None else root / f.depth,
                T_world_camera_gt=None,
                T_world_camera_device=T,
            )
        )
    g = None if man.gravity_world is None else np.asarray(man.gravity_world, float)
    has_depth = any(f.depth for f in man.frames)
    return SourceDataset(
        name="scanbundle",
        scene=man.capture_id,
        root=root,
        width=K.width,
        height=K.height,
        K=np.array([[K.fx, 0, K.cx], [0, K.fy, K.cy], [0, 0, 1.0]]),
        dist=None if K.distortion is None else np.asarray(K.distortion.coeffs, float),
        depth_scale_m=0.001 if has_depth else None,
        depth_source="lidar" if has_depth else "none",
        frames=frames,
        gravity_world_gt=g,
        gravity_source=("imu_accel" if man.gravity_source == "none" else man.gravity_source)
        if g is not None
        else "none",
        gt_geometry=None,
        has_gt=False,
        device_pose_source=man.pose_source,
        notes=[str(i) for i in issues if i.level == "warning"]
        + [f"scan bundle {man.schema_version}, tier {man.tier}, device {man.device.model}"],
    )
