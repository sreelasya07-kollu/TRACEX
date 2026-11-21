"""Multi-view-stereo depth via OpenMVS (docker image `room-openmvs:2.4.0`, CPU).

Pipeline: workspace sparse/0 -> InterfaceCOLMAP -> scene.mvs -> DensifyPointCloud
--fusion-mode 1 (export per-view depth maps, no fusion) -> read the .dmap files.
Depth is metric exactly when the sparse model is (Phase 2 output), otherwise in SfM units.
Output per frame: float32 depth (m) at the OpenMVS depth-map resolution + confidence.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

IMAGE = "room-openmvs:2.4.0"
BIN = "/opt/openmvs/bin/OpenMVS"


@dataclass
class MVSParams:
    resolution_level: int | None = None  # None: derived from the Phase 3 downscale (log2)
    number_views: int = 5
    max_threads: int = 4
    min_resolution: int = 160
    timeout_s: int = 3600


def _docker(args: list[str], mounts: dict[Path, str], timeout: int) -> str:
    cmd = [
        "docker",
        "run",
        "--rm",
        *(["-u", f"{os.getuid()}:{os.getgid()}"] if hasattr(os, "getuid") else []),
    ]
    for host, ctr in mounts.items():
        cmd += ["-v", f"{host.resolve()}:{ctr}"]
    cmd += [IMAGE, *args]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(
            f"{' '.join(args[:2])} failed ({r.returncode}):\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}"
        )
    return r.stdout + r.stderr


def run_openmvs_depth(ws: Path, out: Path, p: MVSParams, downscale: int = 1) -> Path:
    """Returns the folder with depth*.dmap files."""
    out.mkdir(parents=True, exist_ok=True)
    level = p.resolution_level if p.resolution_level is not None else int(round(np.log2(downscale)))
    mounts = {ws.resolve(): "/ws:ro", out.resolve(): "/out"}
    stage = out / "colmap" / "sparse"  # InterfaceCOLMAP wants <input>/sparse/{cameras,...}.bin
    stage.mkdir(parents=True, exist_ok=True)
    for n in ("cameras.bin", "images.bin", "points3D.bin"):
        shutil.copy(ws / "sparse" / "0" / n, stage / n)
    _docker(
        [
            f"{BIN}/InterfaceCOLMAP",
            "-i",
            "/out/colmap",
            "--image-folder",
            "/ws/images/",
            "-o",
            "/out/scene.mvs",
            "-w",
            "/out",
        ],
        mounts,
        600,
    )
    _docker(
        [
            f"{BIN}/DensifyPointCloud",
            "scene.mvs",
            "-w",
            "/out",
            "--fusion-mode",
            "1",
            "--resolution-level",
            str(level),
            "--min-resolution",
            str(p.min_resolution),
            "--number-views",
            str(p.number_views),
            "--max-threads",
            str(p.max_threads),
        ],
        mounts,
        p.timeout_s,
    )
    return out


def read_dmap(path: Path) -> dict:
    """OpenMVS .dmap reader (format: MVS Interface.h `DepthMap`)."""
    with open(path, "rb") as f:
        if f.read(2) != b"DR":
            raise ValueError(f"{path}: not a dmap file")
        ctype = f.read(1)[0]
        f.read(1)
        w, h = np.frombuffer(f.read(8), "<u4")
        dw, dh = np.frombuffer(f.read(8), "<u4")
        dmin, dmax = np.frombuffer(f.read(8), "<f4")
        name = f.read(int(np.frombuffer(f.read(2), "<u2")[0])).decode()
        nv = int(np.frombuffer(f.read(4), "<u4")[0])
        views = np.frombuffer(f.read(4 * nv), "<u4")
        K = np.frombuffer(f.read(72), "<f8").reshape(3, 3)
        R = np.frombuffer(f.read(72), "<f8").reshape(3, 3)
        C = np.frombuffer(f.read(24), "<f8")
        n = int(dw * dh)
        depth = np.frombuffer(f.read(4 * n), "<f4").reshape(int(dh), int(dw)).copy()
        out = {
            "image_name": name,
            "image_size": (int(w), int(h)),
            "depth_size": (int(dw), int(dh)),
            "depth_range": (float(dmin), float(dmax)),
            "views": views,
            "K": K,
            "R": R,
            "C": C,
            "depth": depth,
            "conf": None,
        }
        if ctype in (3, 7, 11, 15):
            f.read(12 * n)  # normals
        if ctype in (5, 7, 13, 15):
            out["conf"] = np.frombuffer(f.read(4 * n), "<f4").reshape(int(dh), int(dw)).copy()
    return out


def load_mvs_depths(
    folder: Path, size_wh: tuple[int, int]
) -> dict[str, tuple[np.ndarray, np.ndarray | None]]:
    """frame id (image stem) -> (depth float32 at `size_wh`, 0 = invalid; confidence or None)."""
    res = {}
    for f in sorted(folder.glob("depth*.dmap")):
        d = read_dmap(f)
        depth = d["depth"]
        conf = d["conf"]
        if (depth.shape[1], depth.shape[0]) != size_wh:
            depth = cv2.resize(depth, size_wh, interpolation=cv2.INTER_NEAREST)
            conf = (
                None if conf is None else cv2.resize(conf, size_wh, interpolation=cv2.INTER_NEAREST)
            )
        depth[~np.isfinite(depth)] = 0
        res[Path(d["image_name"]).stem] = (depth.astype(np.float32), conf)
    return res
