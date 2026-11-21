"""Phase 1: dataset -> workspace (docs/INTERFACES.md §1)."""

from __future__ import annotations

import json
from dataclasses import asdict
from pathlib import Path

import cv2
import numpy as np
from scipy.spatial.transform import Rotation

from formats.models import FramePrior, Intrinsics, PoseNoise, Priors, WorkspaceMeta
from spatial.geometry import rotation_aligning
from spatial.io.adapters import ADAPTER_VERSION, LOADERS, SourceDataset
from spatial.io.keyframes import KeyframeParams, select_keyframes, sharpness
from spatial.stage import record_stage, sha256_file

NOISE_PRESETS = {
    # Random-walk drift proportional to distance travelled (VIO-like: yaw + translation drift,
    # gravity stays observable so roll/pitch don't drift) + per-frame white jitter.
    "vio_small": dict(
        trans_drift_per_m=0.01, yaw_drift_deg_per_m=0.5, jitter_trans_m=0.002, jitter_rot_deg=0.1
    ),
    "vio_large": dict(
        trans_drift_per_m=0.03, yaw_drift_deg_per_m=2.0, jitter_trans_m=0.005, jitter_rot_deg=0.3
    ),
}


def simulate_vio_prior(T_gt: list[np.ndarray], gravity: np.ndarray, preset: str, seed: int):
    p = NOISE_PRESETS[preset]
    rng = np.random.default_rng(seed)
    up = -gravity / np.linalg.norm(gravity)
    drift_t, yaw = np.zeros(3), 0.0
    out = []
    for k, T in enumerate(T_gt):
        if k:
            d = np.linalg.norm(T[:3, 3] - T_gt[k - 1][:3, 3])
            drift_t = drift_t + rng.normal(0, p["trans_drift_per_m"] * d, 3)
            yaw += rng.normal(0, np.radians(p["yaw_drift_deg_per_m"]) * d)
        D = np.eye(4)
        D[:3, :3] = Rotation.from_rotvec(up * yaw).as_matrix()
        D[:3, 3] = drift_t
        J = np.eye(4)
        J[:3, :3] = Rotation.from_rotvec(
            rng.normal(0, np.radians(p["jitter_rot_deg"]), 3)
        ).as_matrix()
        J[:3, 3] = rng.normal(0, p["jitter_trans_m"], 3)
        out.append(D @ T @ J)
    return out, PoseNoise(preset=preset, seed=seed, **p)


def scale_intrinsics(K: np.ndarray, s: float) -> np.ndarray:
    """Pinhole K after resizing the image by factor s (pixel-centre convention:
    x' = (x + 0.5) * s - 0.5), so fx, fy scale by s and c' = (c + 0.5) * s - 0.5."""
    K = K.copy()
    K[0, 0] *= s
    K[1, 1] *= s
    K[0, 2] = (K[0, 2] + 0.5) * s - 0.5
    K[1, 2] = (K[1, 2] + 0.5) * s - 0.5
    return K


def _undistort_maps(ds: SourceDataset):
    if ds.dist is None:
        return None, ds.K
    K_new, _ = cv2.getOptimalNewCameraMatrix(ds.K, ds.dist, (ds.width, ds.height), 0)
    maps = cv2.initUndistortRectifyMap(
        ds.K, ds.dist, None, K_new, (ds.width, ds.height), cv2.CV_32FC1
    )
    return maps, K_new


def check_prior(ds: SourceDataset, dataset: str, prior: str) -> None:
    """Raise ValueError when the requested prior is unavailable for this dataset."""
    if not ds.has_gt and prior in ("gt", "gt_noisy"):
        raise ValueError(f"{dataset} has no ground truth: use prior='device' or 'none'")
    if prior == "device" and ds.device_pose_source == "none":
        raise ValueError(f"{dataset} has no device poses: use prior='none'")
    if prior == "gt_noisy" and ds.gravity_world_gt is None:
        raise ValueError("gt_noisy prior needs a gravity direction")


def select_frames(
    ds: SourceDataset, stride: int, max_frames: int | None, keyframes: KeyframeParams | None = None
) -> list:
    """Stride, then (without keyframe selection) cap at max_frames; raise when empty."""
    frames = ds.frames[::stride]
    if keyframes is None:
        frames = frames[:max_frames]
    if not frames:
        raise ValueError("no frames selected")
    return frames


def ingest(
    dataset: str,
    src: Path,
    out: Path,
    *,
    max_frames: int | None = None,
    stride: int = 1,
    downscale: int = 1,
    max_side: int | None = None,
    prior: str = "gt",
    noise_preset: str = "vio_small",
    seed: int = 0,
    keyframes: KeyframeParams | None = None,
) -> Path:
    if max_side is not None and downscale > 1:
        raise ValueError("max_side and downscale > 1 are mutually exclusive")
    if max_side is not None and max_side < 1:
        raise ValueError(f"max_side must be >= 1, got {max_side}")
    params = dict(
        dataset=dataset,
        src=str(src),
        max_frames=max_frames,
        stride=stride,
        downscale=downscale,
        prior=prior,
        noise_preset=noise_preset,
        seed=seed,
        keyframes=None if keyframes is None else asdict(keyframes),
    )
    if max_side is not None:  # opt-in key: stage params of default runs stay unchanged
        params["max_side"] = max_side
    ws = out / "workspace"
    with record_stage("phase1_ingest", out / "phase1", params=params) as rec:
        ds = LOADERS[dataset](src)
        check_prior(ds, dataset, prior)
        frames = select_frames(ds, stride, max_frames, keyframes)
        maps, K = _undistort_maps(ds)
        W, H = ds.width // downscale, ds.height // downscale
        K = K.copy()
        resize = downscale > 1
        if max_side is not None:
            # Opt-in pre-resize to a long side of max_side (never upscales).
            s = max_side / max(ds.width, ds.height)
            if s < 1:
                W, H, resize = round(ds.width * s), round(ds.height * s), True
                K = scale_intrinsics(K, s)
        else:
            # Integer downscale with pixel-centre convention: x' = (x + 0.5)/D - 0.5
            K[0, 0] /= downscale
            K[1, 1] /= downscale
            K[0, 2] = (K[0, 2] + 0.5) / downscale - 0.5
            K[1, 2] = (K[1, 2] + 0.5) / downscale - 0.5
        for d in ["images", "depth", *(["gt"] if ds.has_gt else [])]:
            (ws / d).mkdir(parents=True, exist_ok=True)

        T_gt = [f.T_world_camera_gt for f in frames]
        noise = None
        if prior == "gt":
            T_prior, pose_source = T_gt, "dataset_gt"
        elif prior == "device":
            T_prior, pose_source = [f.T_world_camera_device for f in frames], ds.device_pose_source
        elif prior == "gt_noisy":
            T_prior, noise = simulate_vio_prior(T_gt, ds.gravity_world_gt, noise_preset, seed)
            pose_source = "dataset_gt_noisy"
        else:
            T_prior, pose_source = [None] * len(frames), "none"

        if keyframes is not None:  # sharpness + motion-group selection on the PRIOR poses
            sharp = np.array([sharpness(f.rgb) for f in frames])
            sel = select_keyframes(sharp, T_prior, keyframes)[:max_frames]
            frames = [frames[i] for i in sel]
            T_gt = [T_gt[i] for i in sel]
            T_prior = [T_prior[i] for i in sel]

        fp = []
        for f, Tp in zip(frames, T_prior, strict=True):
            fid = f"{f.index:06d}"
            img = cv2.imread(str(f.rgb), cv2.IMREAD_COLOR)
            if maps is not None:
                img = cv2.remap(img, *maps, cv2.INTER_LINEAR)
            if resize:
                img = cv2.resize(img, (W, H), interpolation=cv2.INTER_AREA)
            cv2.imwrite(str(ws / "images" / f"{fid}.jpg"), img, [cv2.IMWRITE_JPEG_QUALITY, 95])
            depth_rel = None
            if f.depth is not None:
                raw = cv2.imread(str(f.depth), cv2.IMREAD_UNCHANGED).astype(np.float64)
                if maps is not None:
                    raw = cv2.remap(raw, *maps, cv2.INTER_NEAREST)
                if resize:
                    raw = cv2.resize(raw, (W, H), interpolation=cv2.INTER_NEAREST)
                mm = np.clip(np.round(raw * ds.depth_scale_m * 1000), 0, 65535).astype(np.uint16)
                cv2.imwrite(str(ws / "depth" / f"{fid}.png"), mm)
                depth_rel = f"depth/{fid}.png"
            fp.append(
                FramePrior(
                    id=fid,
                    timestamp_ns=f.timestamp_ns,
                    image=f"images/{fid}.jpg",
                    depth=depth_rel,
                    T_world_camera=None if Tp is None else Tp.tolist(),
                )
            )

        g = ds.gravity_world_gt if pose_source != "none" else None
        priors = Priors(
            width=W,
            height=H,
            intrinsics=Intrinsics(fx=K[0, 0], fy=K[1, 1], cx=K[0, 2], cy=K[1, 2]),
            frames=fp,
            pose_source=pose_source,
            pose_noise=noise,
            gravity_world=None if g is None else (g / np.linalg.norm(g)).tolist(),
            gravity_source=ds.gravity_source if g is not None else "none",
            depth_source=ds.depth_source if ds.depth_scale_m else "none",
        )
        (ws / "priors.json").write_text(priors.model_dump_json(indent=1))
        meta = WorkspaceMeta(
            dataset=dataset,
            scene=ds.scene,
            source_path=str(src),
            frame_ids_source=[f.index for f in frames],
            stride=stride,
            max_frames=max_frames,
            downscale=downscale,
            distortion_removed=ds.dist is not None,
            adapter_version=ADAPTER_VERSION,
            notes=ds.notes,
        )
        (ws / "workspace_meta.json").write_text(meta.model_dump_json(indent=1))

        if ds.has_gt:  # Ground truth: evaluation only (docs/INTERFACES.md §0).
            (ws / "gt" / "poses_gt.json").write_text(
                json.dumps(
                    {f"{f.index:06d}": T.tolist() for f, T in zip(frames, T_gt, strict=True)}
                )
            )
            gt_ref = {
                "dataset": dataset,
                "scene": ds.scene,
                "frame": "dataset GT world",
                "gravity_world": None
                if ds.gravity_world_gt is None
                else ds.gravity_world_gt.tolist(),
                "gravity_source": ds.gravity_source,
            }
            if ds.gt_geometry is not None and ds.gt_geometry.exists():
                gt_ref |= {"geometry": str(ds.gt_geometry), "sha256": sha256_file(ds.gt_geometry)}
            (ws / "gt" / "gt_ref.json").write_text(json.dumps(gt_ref, indent=1))

        rec.metrics |= {
            "n_frames": len(fp),
            "width": W,
            "height": H,
            **({"max_side": max_side} if max_side is not None else {}),
            "with_depth": sum(f.depth is not None for f in fp),
        }
        if noise is not None:
            err = [np.linalg.norm(a[:3, 3] - b[:3, 3]) for a, b in zip(T_prior, T_gt, strict=True)]
            rec.metrics |= {
                "prior_pos_err_mean_m": float(np.mean(err)),
                "prior_pos_err_max_m": float(np.max(err)),
            }
        rec.notes += ds.notes
        for p in [ws / "priors.json", ws / "workspace_meta.json"]:
            rec.add_output(p)
    return ws


def gravity_alignment_rotation(gravity_world) -> np.ndarray:
    """Rotation taking the prior world frame to our +Z-up world (gravity -> -Z)."""
    return rotation_aligning(np.asarray(gravity_world, float), np.array([0.0, 0.0, -1.0]))
