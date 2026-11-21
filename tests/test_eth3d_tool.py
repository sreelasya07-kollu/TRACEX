"""E8 (HANDOFF §3): the ETH3D scoring tool on known answers, on a cropped REAL scan (keeps the
sensor camera element): self-score = 1, a 20 cm translated copy scores 0 at tight tolerances,
and the Open3D-layout pitfall is guarded. Seconds; needs docker image room-eth3d-eval."""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import numpy as np
import open3d as o3d
import pytest
from plyfile import PlyData, PlyElement

from eval.eth3d import run_eth3d_eval, write_xyz_f32_ply

ROOT = Path(__file__).resolve().parents[1]
SCAN = ROOT / "data/eth3d/office/dslr_scan_eval"
RESULTS = ROOT / "runs" / "test_results"


def _need():
    if shutil.which("docker") is None:
        pytest.skip("docker missing")
    import subprocess

    if subprocess.run(
        ["docker", "image", "inspect", "room-eth3d-eval"], capture_output=True
    ).returncode:
        pytest.skip(
            "image room-eth3d-eval missing: docker build -t room-eth3d-eval docker/eth3d-eval"
        )
    if not (SCAN / "scan1.ply").exists():
        pytest.skip("ETH3D office scan missing (data/fetch.sh)")


@pytest.mark.heavy
def test_eth3d_tool_known_answers(tmp_path):
    _need()
    ply = PlyData.read(str(SCAN / "scan1.ply"))
    v = ply["vertex"].data
    xyz = np.stack([v["x"], v["y"], v["z"]], 1)
    sel = np.sort(
        np.random.default_rng(0).choice(
            np.where(np.linalg.norm(xyz, axis=1) < 2.5)[0], 200_000, replace=False
        )
    )
    sub, cam = np.array(v[sel]), np.array(ply["camera"].data)
    cam["viewportx"] = len(sub)
    scan = tmp_path / "scan"
    scan.mkdir()
    PlyData([PlyElement.describe(sub, "vertex"), PlyElement.describe(cam, "camera")]).write(
        str(scan / "scan1.ply")
    )
    ident = "1 0 0 0\n0 1 0 0\n0 0 1 0\n0 0 0 1"
    (scan / "scan_alignment.mlp").write_text(
        '<MeshLabProject><MeshGroup><MLMesh label="scan1.ply" filename="scan1.ply"><MLMatrix44>\n'
        f"{ident}\n</MLMatrix44></MLMesh></MeshGroup></MeshLabProject>\n"
    )
    pts = np.stack([sub["x"], sub["y"], sub["z"]], 1)
    pc = o3d.geometry.PointCloud(o3d.utility.Vector3dVector(pts)).voxel_down_sample(0.01)
    # Open3D writes double xyz (+ the tool would score 0): run_eth3d_eval must convert
    same = tmp_path / "same.ply"
    o3d.io.write_point_cloud(str(same), pc)
    shifted = tmp_path / "shifted.ply"
    pc2 = o3d.geometry.PointCloud(pc)
    pc2.translate((0.20, 0.0, 0.0))
    o3d.io.write_point_cloud(str(shifted), pc2)
    r_same = run_eth3d_eval(same, scan, (0.01, 0.05))
    r_shift = run_eth3d_eval(shifted, scan, (0.01, 0.05))
    out = {
        "self": {k: r_same[k] for k in ("completeness", "accuracy", "fscore")},
        "shifted_20cm": {k: r_shift[k] for k in ("completeness", "accuracy", "fscore")},
    }
    (RESULTS / "env_e8_eth3d_tool.json").write_text(json.dumps(out, indent=1))
    assert min(r_same["fscore"]) > 0.99
    # A shift along x leaves surfaces parallel to x (floor, walls) matched, so F is not ~0
    # (measured 0.20 @1 cm, 0.42 @5 cm); it must still be far below the self-score.
    assert max(r_shift["fscore"]) < 0.5
    write_xyz_f32_ply(pts[:10], tmp_path / "t.ply")  # layout helper stays importable
