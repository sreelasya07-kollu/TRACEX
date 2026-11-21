"""Dataset adapters -> a uniform list of frames (Phase 1 input side).

Each adapter yields `SourceFrame`s with absolute file paths, timestamps, the dataset's
calibration and (for evaluation only) the GT pose in the dataset's own world frame.
Convention conversions happen here, once, and are covered by tests/test_phase1.py.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation, Slerp

from spatial.geometry import invert, pose_from_tq_xyzw

ADAPTER_VERSION = "0.1.0"


@dataclass
class SourceFrame:
    index: int  # index in the dataset's own frame list
    timestamp_ns: int
    rgb: Path
    depth: Path | None
    T_world_camera_gt: np.ndarray | None  # dataset GT frame (OpenCV axes); None for real bundles
    T_world_camera_device: np.ndarray | None = None  # device pose (ARKit/ARCore), OpenCV axes


@dataclass
class SourceDataset:
    name: str
    scene: str
    root: Path
    width: int
    height: int
    K: np.ndarray  # 3x3, for the original (possibly distorted) images
    dist: np.ndarray | None  # OpenCV distortion coeffs or None if already pinhole
    depth_scale_m: float | None  # metres per raw depth unit
    depth_source: str  # lidar | kinect | rendered | none
    frames: list[SourceFrame]
    gravity_world_gt: np.ndarray | None = None  # unit, pointing down, dataset GT frame
    gravity_source: str = "none"
    gt_geometry: Path | None = None
    has_gt: bool = True  # False for real captures (Scan Bundles): no ground truth anywhere
    device_pose_source: str = "none"  # arkit | arcore | none (device poses, not GT)
    notes: list[str] = field(default_factory=list)


def load_replica(root: Path) -> SourceDataset:
    """NICE-SLAM rendered Replica: results/frameXXXXXX.jpg, depthXXXXXX.png, traj.txt (c2w rows)."""
    cam = json.loads((root.parent / "cam_params.json").read_text())["camera"]
    traj = np.loadtxt(root / "traj.txt").reshape(-1, 4, 4)
    subset = json.loads((root / "SUBSET.json").read_text())
    frames = []
    for i in subset["frame_ids"]:
        rgb, dep = root / "results" / f"frame{i:06d}.jpg", root / "results" / f"depth{i:06d}.png"
        if rgb.exists() and dep.exists():
            # Rendered sequence without timestamps: assume 30 Hz for bookkeeping only.
            frames.append(SourceFrame(i, int(round(i * 1e9 / 30)), rgb, dep, traj[i]))
    K = np.array([[cam["fx"], 0, cam["cx"]], [0, cam["fy"], cam["cy"]], [0, 0, 1.0]])
    scene = root.name
    culled = root.parent / "cull_replica_mesh" / f"{scene}.ply"
    return SourceDataset(
        "replica",
        scene,
        root,
        cam["w"],
        cam["h"],
        K,
        None,
        1.0 / cam["scale"],
        "rendered",
        frames,
        # Replica meshes are z-up (verified on the floor plane in tests/test_phase1.py).
        gravity_world_gt=np.array([0.0, 0.0, -1.0]),
        gravity_source="dataset_axis",
        gt_geometry=culled if culled.exists() else root.parent / f"{scene}_mesh.ply",
        notes=["timestamps synthetic (30 Hz)", "gravity: Replica +Z up (checked on GT floor)"],
    )


# TUM fr1 calibration (https://cvg.cit.tum.de/data/datasets/rgbd-dataset/file_formats)
TUM_FR1 = dict(
    fx=517.3, fy=516.5, cx=318.6, cy=255.3, dist=[0.2624, -0.9531, -0.0054, 0.0026, 1.1633]
)


def _read_tum_list(path: Path) -> list[list[str]]:
    return [ln.split() for ln in path.read_text().splitlines() if ln and not ln.startswith("#")]


def _interp_pose(ts: np.ndarray, poses: np.ndarray, t: float) -> np.ndarray | None:
    j = np.searchsorted(ts, t)
    if j == 0 or j >= len(ts) or ts[j] - ts[j - 1] > 0.05:
        return None
    a = (t - ts[j - 1]) / (ts[j] - ts[j - 1])
    rot = Slerp([0, 1], Rotation.from_matrix(poses[[j - 1, j], :3, :3]))(a).as_matrix()
    T = np.eye(4)
    T[:3, :3] = rot
    T[:3, 3] = (1 - a) * poses[j - 1, :3, 3] + a * poses[j, :3, 3]
    return T


def load_tum(root: Path, max_dt: float = 0.02) -> SourceDataset:
    """TUM RGB-D: rgb/depth associated by nearest timestamp (≤ max_dt), GT pose interpolated
    at the RGB timestamp. groundtruth.txt is camera-to-world `t q(xyzw)` in the mocap frame."""
    rgb = _read_tum_list(root / "rgb.txt")
    dep = _read_tum_list(root / "depth.txt")
    gt = np.array([[float(x) for x in r] for r in _read_tum_list(root / "groundtruth.txt")])
    gt_ts = gt[:, 0]
    gt_T = np.stack([pose_from_tq_xyzw(r[1:4], r[4:8]) for r in gt])
    dts = np.array([float(r[0]) for r in dep])
    frames, assoc_dt = [], []
    for i, (t_str, rel) in enumerate(rgb):
        t = float(t_str)
        j = int(np.argmin(np.abs(dts - t)))
        T = _interp_pose(gt_ts, gt_T, t)
        if abs(dts[j] - t) > max_dt or T is None:
            continue
        assoc_dt.append(abs(dts[j] - t))
        frames.append(SourceFrame(i, int(round(t * 1e9)), root / rel, root / dep[j][1], T))
    K = np.array([[TUM_FR1["fx"], 0, TUM_FR1["cx"]], [0, TUM_FR1["fy"], TUM_FR1["cy"]], [0, 0, 1]])
    ds = SourceDataset(
        "tum",
        root.name,
        root,
        640,
        480,
        K,
        np.array(TUM_FR1["dist"]),
        1 / 5000,
        "kinect",
        frames,
        notes=[f"rgb-depth association max dt {max(assoc_dt) * 1e3:.1f} ms"],
    )
    g = tum_gravity(root, gt_ts, gt_T)
    if g is not None:
        ds.gravity_world_gt, ds.gravity_source = g["gravity_world"], "imu_accel"
        ds.notes.append(
            f"gravity from accelerometer: axis map {g['axis_map']}, "
            f"angular spread {g['spread_deg']:.2f} deg over {g['n']} samples "
            f"(motion-compensated), {g['angle_to_dataset_z_deg']:.2f} deg from mocap Z"
        )
    return ds


def tum_gravity(root: Path, gt_ts, gt_T, n_max: int = 1500, h: float = 0.05) -> dict | None:
    """Estimate gravity (down) in the mocap world from the Kinect accelerometer.

    Dataset-adapter only: it uses the GT trajectory, standing in for the gravity estimate a
    VIO system (ARKit/ARCore) provides, so it must never run inside the pipeline.
    Model: the accelerometer measures specific force f = a_lin - g. In the world frame,
    up ∝ k·R·M·a_meas - a_lin, where a_lin comes from the GT trajectory (central
    difference, step h), k rescales the uncalibrated Kinect magnitude to 9.81 m/s², and M is
    the accelerometer->optical axis map. M isn't documented, so all 24 proper signed axis
    permutations are tried; we keep the one with the most constant world-frame vector.
    """
    import itertools

    p = root / "accelerometer.txt"
    if not p.exists():
        return None
    acc = np.array([[float(x) for x in r] for r in _read_tum_list(p)])
    k = 9.81 / np.median(np.linalg.norm(acc[:, 1:], axis=1))
    idx = np.linspace(0, len(acc) - 1, min(n_max, len(acc))).astype(int)
    samples = []
    for t, *a in acc[idx]:
        P = [_interp_pose(gt_ts, gt_T, t + d) for d in (-h, 0.0, h)]
        if any(q is None for q in P):
            continue
        a_lin = (P[2][:3, 3] - 2 * P[1][:3, 3] + P[0][:3, 3]) / h**2
        samples.append((P[1][:3, :3], k * np.asarray(a), a_lin))
    best = None
    for perm in itertools.permutations(range(3)):
        for signs in itertools.product([1, -1], repeat=3):
            M = np.zeros((3, 3))
            for r, (c, s) in enumerate(zip(perm, signs, strict=True)):
                M[r, c] = s
            if np.linalg.det(M) < 0:
                continue
            w = np.array([R @ (M @ a) - al for R, a, al in samples])
            w /= np.linalg.norm(w, axis=1, keepdims=True)
            m = w.mean(0)
            m /= np.linalg.norm(m)
            spread = np.degrees(np.arccos(np.clip(w @ m, -1, 1))).mean()
            if best is None or spread < best[0]:
                best = (spread, M, m)
    spread, M, up = best
    return {
        "gravity_world": -up,
        "axis_map": M.astype(int).tolist(),
        "spread_deg": float(spread),
        "n": len(samples),
        "angle_to_dataset_z_deg": float(np.degrees(np.arccos(abs(up[2])))),
    }


def load_eth3d(root: Path) -> SourceDataset:
    """ETH3D DSLR undistorted: COLMAP text model; images.txt stores world-to-camera (qw..)."""
    cal = root / "dslr_calibration_undistorted"
    cam = [
        ln.split()
        for ln in (cal / "cameras.txt").read_text().splitlines()
        if ln and not ln.startswith("#")
    ][0]
    W, H, fx, fy, cx, cy = int(cam[2]), int(cam[3]), *map(float, cam[4:8])
    lines = [ln for ln in (cal / "images.txt").read_text().splitlines() if not ln.startswith("#")]
    frames = []
    for ln in lines[0::2]:
        s = ln.split()
        if len(s) < 10:
            continue
        qw, qx, qy, qz, tx, ty, tz = map(float, s[1:8])
        T_cw = pose_from_tq_xyzw([tx, ty, tz], [qx, qy, qz, qw])
        frames.append(SourceFrame(int(s[0]), 0, root / "images" / s[9], None, invert(T_cw)))
    frames.sort(key=lambda f: f.rgb.name)
    for k, f in enumerate(frames):
        f.timestamp_ns = k  # no timestamps in ETH3D; keep order
    K = np.array([[fx, 0, cx], [0, fy, cy], [0, 0, 1]])
    return SourceDataset(
        "eth3d",
        root.name,
        root,
        W,
        H,
        K,
        None,
        None,
        "none",
        frames,
        gt_geometry=root / "dslr_scan_eval" / "scan_alignment.mlp",
        notes=["no gravity, no timestamps, no sensor depth"],
    )


def load_scanbundle(root: Path) -> SourceDataset:
    from spatial.io.scanbundle import load_scanbundle as _load  # lazy: avoids an import cycle

    return _load(root)


LOADERS = {
    "replica": load_replica,
    "tum": load_tum,
    "eth3d": load_eth3d,
    "scanbundle": load_scanbundle,
}
