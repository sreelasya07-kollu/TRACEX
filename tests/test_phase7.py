"""Phase 7 (HANDOFF P7): sparse model for Part 4. ETH3D office: >= 95 % registered, mean
reprojection < 1.0 px, and the model loads in OpenMVS (InterfaceCOLMAP converts it)."""

from __future__ import annotations

import json
import re
import shutil
from pathlib import Path

import pytest

from recon import openmvs
from recon.sparse import SparseScene, check_scene, export_for_openmvs

ROOT = Path(__file__).resolve().parents[1]
WS = ROOT / "runs/dev_p2_eth3d_office/workspace"
RESULTS = ROOT / "runs" / "test_results"


@pytest.mark.heavy
def test_eth3d_sparse_checks_and_openmvs_load(tmp_path):
    if not (WS / "sparse/0/images.bin").exists():
        pytest.skip("needs runs/dev_p2_eth3d_office (Phase 1+2)")
    scene = SparseScene.from_workspace(WS)
    n_images = len(list((WS / "images").glob("*.jpg")))
    c = check_scene(scene, n_expected=n_images)
    out = {"check": c}
    if openmvs.available():
        mvs = export_for_openmvs(scene, tmp_path)
        log = (tmp_path / "logs" / "InterfaceCOLMAP.log").read_text()
        m = re.search(r"(\d+) images.*?(\d+) points", log.replace("\n", " "))
        out["openmvs_scene_bytes"] = mvs.stat().st_size
        out["openmvs_log_summary"] = m.group(0) if m else None
        assert mvs.stat().st_size > 1000
        assert shutil.which("docker")
    else:
        out["openmvs"] = "SKIPPED: docker image room-openmvs:2.4.0 not available"
    (RESULTS / "phase7_eth3d.json").write_text(json.dumps(out, indent=1))
    assert c["reg_ratio"] >= 0.95 and c["mean_reproj_px"] < 1.0
    if openmvs.available():
        assert out["openmvs_log_summary"] is not None, "OpenMVS did not report loaded images"


@pytest.mark.light
def test_docker_command_has_memory_cap_and_unique_name(tmp_path):
    a = openmvs.docker_cmd("DensifyPointCloud", ["-i", "x"], tmp_path, tmp_path, memory_gb=3.5)
    b = openmvs.docker_cmd("DensifyPointCloud", ["-i", "x"], tmp_path, None)
    assert a[a.index("--memory") + 1] == "3.5g" and a[a.index("--memory-swap") + 1] == "3.5g"
    assert "--memory" not in b
    assert a[a.index("--name") + 1] != b[b.index("--name") + 1]  # unique: killable on timeout
    assert a[-3] == "-w" and a[-2] == "/out" or a[-2:] == ["-w", "/out"]
