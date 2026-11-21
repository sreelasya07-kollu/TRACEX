"""ETH3D official multi-view evaluation (docker image `room-eth3d-eval`, docker/eth3d-eval).

The tool scores a reconstruction PLY (in the ETH3D GT frame = the frame of the undistorted DSLR
images / COLMAP model) against the laser scans listed in scan_alignment.mlp. Tolerances in m."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import numpy as np
import open3d as o3d

IMAGE = "room-eth3d-eval"


def read_mlp_transforms(mlp: Path) -> dict[str, np.ndarray]:
    import xml.etree.ElementTree as ET

    out = {}
    for m in ET.parse(mlp).getroot().iter("MLMesh"):
        mat = np.array(m.find("MLMatrix44").text.split(), float).reshape(4, 4)
        out[m.get("filename")] = mat
    return out


def gt_cloud_in_gt_frame(scan_dir: Path, voxel: float = 0.01) -> o3d.geometry.PointCloud:
    """Union of the GT scans transformed by scan_alignment.mlp, voxel-downsampled."""
    mats = read_mlp_transforms(scan_dir / "scan_alignment.mlp")
    total = o3d.geometry.PointCloud()
    for name, M in mats.items():
        p = o3d.io.read_point_cloud(str(scan_dir / name))
        p = p.voxel_down_sample(voxel)
        p.transform(M)
        total += p
    return total


def write_xyz_f32_ply(points: np.ndarray, path: Path) -> None:
    """The ETH3D tool's PCL reader only scores correctly on a plain float32 x,y,z PLY: Open3D's
    default layout (double coordinates, normals, colours) makes every metric 0 without an error
    (found by testing; see tests/test_eth3d_tool.py)."""
    from plyfile import PlyData, PlyElement

    q = np.asarray(points, np.float32)
    e = np.empty(len(q), dtype=[("x", "<f4"), ("y", "<f4"), ("z", "<f4")])
    e["x"], e["y"], e["z"] = q.T
    PlyData([PlyElement.describe(e, "vertex")], text=False).write(str(path))


def run_eth3d_eval(
    recon_ply: Path,
    scan_dir: Path,
    tolerances=(0.01, 0.02, 0.05, 0.1, 0.2),
    timeout: int = 3600,
) -> dict:
    """Returns {"tolerances": [...], "completeness": [...], "accuracy": [...], "fscore": [...],
    "raw": stdout}. Parsed from the tool's stdout."""
    recon_ply, scan_dir = recon_ply.resolve(), scan_dir.resolve()
    clean = recon_ply.with_name(recon_ply.stem + ".f32xyz.ply")
    write_xyz_f32_ply(np.asarray(o3d.io.read_point_cloud(str(recon_ply)).points), clean)
    try:
        return _run(clean, scan_dir, tolerances, timeout)
    finally:
        clean.unlink(missing_ok=True)


def _run(recon_ply: Path, scan_dir: Path, tolerances, timeout: int) -> dict:
    work = recon_ply.parent
    cmd = [
        "docker",
        "run",
        "--rm",
        *(["-u", f"{os.getuid()}:{os.getgid()}"] if hasattr(os, "getuid") else []),
        "-v",
        f"{work}:/recon:ro",
        "-v",
        f"{scan_dir}:/scan:ro",
        IMAGE,
        "--reconstruction_ply_path",
        f"/recon/{recon_ply.name}",
        "--ground_truth_mlp_path",
        "/scan/scan_alignment.mlp",
        "--tolerances",
        ",".join(str(t) for t in tolerances),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(
            f"ETH3D eval failed ({r.returncode}):\n{r.stdout[-800:]}\n{r.stderr[-800:]}"
        )
    return parse_output(r.stdout, tolerances) | {"raw": r.stdout}


def parse_output(text: str, tolerances) -> dict:
    """The tool prints three lines: 'Completenesses: v1 v2 ...', 'Accuracies: ...',
    'F1-scores: ...' (one value per tolerance)."""

    def row(label):
        m = re.search(rf"{label}[^:]*:\s*([0-9eE+.\- nan]+)", text)
        if not m:
            raise ValueError(f"cannot parse '{label}' from tool output:\n{text[-600:]}")
        return [float(x) for x in m.group(1).split()]

    out = {
        "tolerances": list(tolerances),
        "completeness": row("Completenesses"),
        "accuracy": row("Accuracies"),
        "fscore": row("F1-scores"),
    }
    n = len(tolerances)
    if not all(len(out[k]) == n for k in ("completeness", "accuracy", "fscore")):
        raise ValueError(f"expected {n} values per metric, got {out}")
    return out
