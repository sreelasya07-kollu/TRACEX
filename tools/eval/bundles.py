"""Reference Scan Bundle writer (what the capture app must produce), sample bundles per tier from
the synthetic room, and a corpus of intentionally broken bundles with the issue codes each must
raise (HANDOFF §6.2). Shared with the mobile team as the executable form of docs/SCAN_BUNDLE.md."""

from __future__ import annotations

import json
import shutil
from collections.abc import Callable
from pathlib import Path

import cv2
import numpy as np
import open3d as o3d

from eval.synthetic_room import camera_model, cameras, render_depth, room_mesh
from formats.scanbundle import SCHEMA_VERSION, sha256

_FLIP = np.diag([1.0, -1.0, -1.0, 1.0])


def write_bundle(
    out: Path,
    *,
    images: list[np.ndarray],
    poses_cv: list[np.ndarray | None],
    K: np.ndarray,
    tier: str,
    depths_mm: list[np.ndarray | None] | None = None,
    camera_convention: str = "arkit",
    world_up: str = "+Z",
    gravity_world: list[float] | None = (0.0, 0.0, -1.0),
    pose_source: str = "arkit",
    timestamps_ns: list[int] | None = None,
    capture_id: str = "sample",
    consent: bool = True,
) -> Path:
    """`poses_cv` are OpenCV-axes T_world_camera; they are stored in `camera_convention`."""
    out.mkdir(parents=True, exist_ok=True)
    (out / "images").mkdir(exist_ok=True)
    H, W = images[0].shape[:2]
    n = len(images)
    ts = timestamps_ns or [k * 33_333_333 for k in range(n)]
    frames, files = [], {}
    for k in range(n):
        fid = f"{k:06d}"
        cv2.imwrite(str(out / "images" / f"{fid}.jpg"), images[k], [cv2.IMWRITE_JPEG_QUALITY, 95])
        rel_d = None
        if depths_mm is not None and depths_mm[k] is not None:
            (out / "depth").mkdir(exist_ok=True)
            rel_d = f"depth/{fid}.png"
            cv2.imwrite(str(out / rel_d), depths_mm[k].astype(np.uint16))
        T = poses_cv[k]
        if T is not None and camera_convention in ("arkit", "opengl"):
            T = T @ _FLIP
        frames.append(
            {
                "id": fid,
                "timestamp_ns": ts[k],
                "image": f"images/{fid}.jpg",
                "depth": rel_d,
                "T_world_camera": None if T is None else np.asarray(T).tolist(),
            }
        )
    for p in sorted(out.rglob("*")):
        if p.is_file() and p.name != "manifest.json":
            files[p.relative_to(out).as_posix()] = sha256(p)
    man = {
        "schema_version": SCHEMA_VERSION,
        "capture_id": capture_id,
        "created": "2026-11-14T12:00:00+00:00",
        "device": {"model": "synthetic", "os": "none", "app_version": "0.0.0"},
        "tier": tier,
        "units": "metres",
        "coordinates": {"world_up": world_up, "camera": camera_convention},
        "gravity_world": None if gravity_world is None else list(gravity_world),
        "gravity_source": "arkit" if gravity_world is not None else "none",
        "pose_source": pose_source,
        "intrinsics": {
            "fx": float(K[0, 0]),
            "fy": float(K[1, 1]),
            "cx": float(K[0, 2]),
            "cy": float(K[1, 2]),
            "width": W,
            "height": H,
            "distortion": None,
        },
        "frames": frames,
        "consent": {"scan_processing": consent, "training_use": False, "faces_blurred": True},
        "quality_report": None,
        "files": files,
    }
    (out / "manifest.json").write_text(json.dumps(man, indent=1))
    return out


def sample_bundle(out: Path, tier: str = "A", n: int = 24, seed: int = 0) -> Path:
    """Synthetic-room bundle (the 24 ring stations, 5 fps: a plausible walk-around trajectory).
    Tier A has ray-cast depth + poses, B poses only, C RGB only."""
    K, W, H = camera_model()
    poses = cameras(seed)[:n]
    rng = np.random.default_rng(seed)
    images = [
        np.clip(110 + 60 * rng.standard_normal((H, W, 1)) + np.zeros((H, W, 3)), 0, 255).astype(
            np.uint8
        )
        for _ in poses
    ]
    depths = None
    if tier == "A":
        scene = o3d.t.geometry.RaycastingScene()
        scene.add_triangles(o3d.t.geometry.TriangleMesh.from_legacy(room_mesh()))
        depths = [
            np.clip(np.round(render_depth(scene, T, K, W, H) * 1000), 0, 65535) for T in poses
        ]
    return write_bundle(
        out,
        images=images,
        poses_cv=list(poses) if tier != "C" else [None] * len(poses),
        K=K,
        tier=tier,
        depths_mm=depths,
        pose_source="arkit" if tier != "C" else "none",
        gravity_world=(0.0, 0.0, -1.0) if tier != "C" else None,
        timestamps_ns=[k * 200_000_000 for k in range(len(poses))],
        capture_id=f"synthetic-tier-{tier}",
    )


# ---- intentionally broken bundles --------------------------------------------------------


def _edit(root: Path, fn: Callable[[dict], None]) -> None:
    p = root / "manifest.json"
    m = json.loads(p.read_text())
    fn(m)
    p.write_text(json.dumps(m))


def _teleport(m):
    T = np.asarray(m["frames"][10]["T_world_camera"])
    T[:3, 3] += [50.0, 0, 0]
    m["frames"][10]["T_world_camera"] = T.tolist()


def _swap_ts(m):
    a, b = m["frames"][5], m["frames"][6]
    a["timestamp_ns"], b["timestamp_ns"] = b["timestamp_ns"], a["timestamp_ns"]


def _gap(m):
    for f in m["frames"][15:]:
        f["timestamp_ns"] += 10_000_000_000


def _nan_pose(m):
    m["frames"][3]["T_world_camera"][0][0] = float("nan")


def _no_depth(m):
    for f in m["frames"]:
        f["depth"] = None


def _small_image(root: Path):
    p = root / "images" / "000000.jpg"
    cv2.imwrite(str(p), np.zeros((10, 10, 3), np.uint8))
    _edit(root, lambda m: m["files"].__setitem__("images/000000.jpg", sha256(p)))


def _rm(root: Path, rel: str):
    (root / rel).unlink()


def _append(root: Path, rel: str):
    with open(root / rel, "ab") as f:
        f.write(b"x")


# name -> (mutation(root), error codes that must appear, warning codes that must appear)
BROKEN: dict[str, tuple[Callable[[Path], None], set[str], set[str]]] = {
    "no_manifest": (lambda r: _rm(r, "manifest.json"), {"E_NO_MANIFEST"}, set()),
    "bad_json": (lambda r: (r / "manifest.json").write_text("{not json"), {"E_SCHEMA"}, set()),
    "wrong_version": (
        lambda r: _edit(r, lambda m: m.__setitem__("schema_version", "1.0.0")),
        {"E_VERSION"},
        set(),
    ),
    "no_consent": (
        lambda r: _edit(r, lambda m: m["consent"].__setitem__("scan_processing", False)),
        {"E_CONSENT"},
        set(),
    ),
    "corrupt_image": (lambda r: _append(r, "images/000002.jpg"), {"E_CHECKSUM"}, set()),
    "missing_depth": (lambda r: _rm(r, "depth/000004.png"), {"E_FILE_MISSING"}, set()),
    "pose_teleport": (lambda r: _edit(r, _teleport), {"E_POSE_TELEPORT"}, set()),
    "nan_pose": (lambda r: _edit(r, _nan_pose), {"E_SCHEMA"}, set()),
    "timestamps_swapped": (lambda r: _edit(r, _swap_ts), {"E_TIMESTAMPS"}, set()),
    "frame_gap": (lambda r: _edit(r, _gap), set(), {"W_FRAME_GAP"}),
    "too_few_frames": (
        lambda r: _edit(r, lambda m: m.__setitem__("frames", m["frames"][:5])),
        {"E_FRAME_COUNT"},
        set(),
    ),
    "tier_a_without_depth": (lambda r: _edit(r, _no_depth), {"E_TIER"}, set()),
    "bad_gravity": (
        lambda r: _edit(r, lambda m: m.__setitem__("gravity_world", [0, 0, 0])),
        {"E_GRAVITY"},
        set(),
    ),
    "image_size_mismatch": (_small_image, {"E_SIZE"}, set()),
    "duplicate_ids": (
        lambda r: _edit(r, lambda m: m["frames"][1].__setitem__("id", m["frames"][0]["id"])),
        {"E_DUP_ID"},
        set(),
    ),
    "unlisted_file": (lambda r: (r / "notes.txt").write_text("hi"), set(), {"W_UNLISTED_FILE"}),
}


def make_broken(valid: Path, out_root: Path) -> dict[str, Path]:
    """Copies `valid` once per variant and applies the mutation. Returns name -> bundle path."""
    res = {}
    for name, (mut, _, _) in BROKEN.items():
        d = out_root / name
        shutil.copytree(valid, d)
        mut(d)
        res[name] = d
    return res
