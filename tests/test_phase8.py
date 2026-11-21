"""Phase 8 (HANDOFF P8): OpenMVS dense + mesh + texture on ETH3D office, scored with the official
ETH3D tool and by held-out-view rendering. Held-out: every 5th image is removed from the sparse
model BEFORE densification/texturing. Output kept in runs/dev_p8_eth3d (input to P10).
Targets: dense F@5cm >= 0.70, mesh F@5cm >= 0.65, textured held-out PSNR >= 18 dB.
Needs docker images room-openmvs:2.4.0 and room-eth3d-eval; CPU only; ~10-30 min."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import open3d as o3d
import pytest

from eval.eth3d import run_eth3d_eval
from eval.trajectory import ate  # noqa: F401  (documented alignment helper)
from recon import openmvs
from recon.dense import DenseParams, run_dense_mesh_texture
from recon.evaluate import heldout_render_metrics, split_names
from recon.sparse import SparseScene
from spatial.geometry import umeyama  # noqa: E402  (test-only alignment of SfM -> GT frame)

ROOT = Path(__file__).resolve().parents[1]
WS = ROOT / "runs/dev_p2_eth3d_office/workspace"
SCAN = ROOT / "data/eth3d/office/dslr_scan_eval"
OUT = ROOT / "runs/dev_p8_eth3d"  # all images: geometry scored with the ETH3D tool
OUT_HELD = ROOT / "runs/dev_p8_eth3d_heldout"  # every 5th image removed: texture PSNR
RESULTS = ROOT / "runs/test_results"
LEVEL = 0  # full workspace resolution (1555 x 1036)


def _sim3_to_gt() -> tuple[float, np.ndarray, np.ndarray]:
    import pycolmap

    gt = json.loads((WS / "gt/poses_gt.json").read_text())
    rec = pycolmap.Reconstruction(WS / "sparse/0")
    P, G = [], []
    for im in rec.images.values():
        k = Path(im.name).stem
        if k in gt:
            P.append(im.cam_from_world().inverse().matrix()[:, 3])
            G.append(np.asarray(gt[k])[:3, 3])
    return umeyama(np.array(P), np.array(G), with_scale=True)


def _to_gt_frame(pts: np.ndarray, s, R, t) -> np.ndarray:
    return s * pts @ R.T + t


@pytest.mark.heavy
@pytest.mark.longrun(hours=0.5)
def test_eth3d_dense_mesh_texture(request):
    if not (WS / "sparse/0/images.bin").exists() or not SCAN.exists():
        pytest.skip("needs ETH3D office data + Phase 2 workspace")
    if not openmvs.available():
        pytest.skip("docker image room-openmvs:2.4.0 missing")
    scene = SparseScene.from_workspace(WS)
    names = sorted(p.name for p in (WS / "images").glob("*.jpg"))
    train, held = split_names(names, 5)
    if not (OUT / "run.json").exists():  # geometry run: every image
        run = run_dense_mesh_texture(
            scene, OUT, DenseParams(resolution_level=LEVEL, max_threads=3), None
        )
        (OUT / "run.json").write_text(json.dumps(run, indent=1, default=str))
    run = json.loads((OUT / "run.json").read_text())
    if not (OUT_HELD / "run.json").exists():  # texture run: held-out images excluded
        runh = run_dense_mesh_texture(
            scene, OUT_HELD, DenseParams(resolution_level=LEVEL, max_threads=3), train
        )
        (OUT_HELD / "run.json").write_text(json.dumps(runh, indent=1, default=str))
    run_held = json.loads((OUT_HELD / "run.json").read_text())

    s, R, t = _sim3_to_gt()
    res = {"sim3_scale": float(s), "n_train": len(train), "n_heldout": len(held), "run": run}  # noqa: E501
    for tag, path in [("dense", OUT / "dense/scene_dense.ply"), ("mesh", OUT / "mesh/mesh.ply")]:
        if tag == "dense":
            pts = np.asarray(o3d.io.read_point_cloud(str(path)).points)
        else:  # sample the mesh surface (2 cm density) as the tool scores point clouds
            m = o3d.io.read_triangle_mesh(str(path))
            pts = np.asarray(m.sample_points_uniformly(400_000).points)
        p = o3d.geometry.PointCloud(o3d.utility.Vector3dVector(_to_gt_frame(pts, s, R, t)))
        ply = OUT / f"{tag}_in_gt_frame.ply"
        o3d.io.write_point_cloud(str(ply), p)
        res[tag] = run_eth3d_eval(ply, SCAN)
        ply.unlink()  # reproducible from the outputs; saves disk
    res["run_heldout"] = run_held
    obj = OUT_HELD / run_held["outputs"]["textured_obj"]
    res["heldout"] = heldout_render_metrics(
        obj, scene, held, 1 / 2**LEVEL, OUT_HELD / "heldout_renders"
    )
    (RESULTS / "phase8_eth3d.json").write_text(json.dumps(res, indent=1, default=str))
    f5 = lambda r: r["fscore"][r["tolerances"].index(0.05)]  # noqa: E731
    assert f5(res["dense"]) >= 0.70, res["dense"]["fscore"]
    assert f5(res["mesh"]) >= 0.65, res["mesh"]["fscore"]
    assert res["heldout"]["mean_psnr_covered"] >= 18.0
