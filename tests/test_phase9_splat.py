"""Phase 9 (HANDOFF P9). CPU tests: initialisation maths, 3DGS PLY layout round-trip, dataset
camera conventions on a real SpatialMap, honest failure without CUDA. The training test is
marked `gpu` and is SKIPPED here (pending for the GPU machine); targets are HANDOFF §5 P9."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
from plyfile import PlyData

from recon.splat.data import SH_C0, init_gaussians, load_dataset, scale_K, split_heldout
from recon.splat.plyio import read_gaussian_ply, write_gaussian_ply
from recon.splat.train import GpuRequired, SplatJob, cuda_available, train_splat

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "runs" / "test_results"
REPLICA_SM = ROOT / "runs/dev_p6_replica/spatial_map"


@pytest.mark.light
def test_init_gaussians_scale_colour_and_shapes():
    g = np.linspace(0, 0.9, 10)
    xyz = np.stack(np.meshgrid(g, g, g), -1).reshape(-1, 3)  # spacing 0.1 m
    rgb = np.random.default_rng(0).uniform(0, 1, (len(xyz), 3))
    p = init_gaussians(xyz, rgb, sh_degree=3)
    assert p["shN"].shape == (1000, 15, 3) and p["sh0"].shape == (1000, 1, 3)
    # interior points: 3 nearest neighbours are all at 0.1 m -> scale exp(log) = 0.1
    assert abs(float(np.exp(p["scales"][555, 0])) - 0.1) < 1e-3
    np.testing.assert_allclose(p["sh0"][:, 0] * SH_C0 + 0.5, rgb, atol=1e-6)
    assert np.allclose(np.linalg.norm(p["quats"], axis=1), 1, atol=1e-6)
    assert np.allclose(1 / (1 + np.exp(-p["opacities"])), 0.1, atol=1e-6)


@pytest.mark.light
def test_ply_layout_and_roundtrip(tmp_path):
    rng = np.random.default_rng(1)
    n, k = 50, 15
    p = {
        "means": rng.normal(size=(n, 3)),
        "scales": rng.normal(size=(n, 3)),
        "quats": rng.normal(size=(n, 4)),
        "opacities": rng.normal(size=n),
        "sh0": rng.normal(size=(n, 1, 3)),
        "shN": rng.normal(size=(n, k, 3)),
    }
    p = {a: b.astype(np.float32) for a, b in p.items()}
    write_gaussian_ply(tmp_path / "g.ply", p)
    names = PlyData.read(str(tmp_path / "g.ply"))["vertex"].data.dtype.names
    assert names[:9] == ("x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2")
    assert names[9] == "f_rest_0" and names[9 + 3 * k - 1] == f"f_rest_{3 * k - 1}"
    assert names[-8:] == (
        "opacity",
        "scale_0",
        "scale_1",
        "scale_2",
        "rot_0",
        "rot_1",
        "rot_2",
        "rot_3",
    )
    v = PlyData.read(str(tmp_path / "g.ply"))["vertex"]
    np.testing.assert_array_equal(v["f_rest_0"], p["shN"][:, 0, 0])  # R, coefficient 0
    np.testing.assert_array_equal(v["f_rest_15"], p["shN"][:, 0, 1])  # channel-major: G starts at K
    q = read_gaussian_ply(tmp_path / "g.ply")
    for key in p:
        np.testing.assert_array_equal(q[key], p[key])


@pytest.mark.light
def test_split_and_intrinsics_scaling():
    tr, he = split_heldout([f"{i:06d}" for i in range(40)], 8)
    assert len(he) == 5 and not set(tr) & set(he) and len(tr) + len(he) == 40
    K = np.array([[300.0, 0, 299.5], [0, 300.0, 169.5], [0, 0, 1]])
    K2 = scale_K(K, 0.5)  # 600x340 -> 300x170: centre of the image stays the centre
    assert K2[0, 2] == pytest.approx(149.5) and K2[1, 2] == pytest.approx(84.5) and K2[0, 0] == 150


@pytest.mark.light
def test_train_refuses_without_cuda(tmp_path):
    if cuda_available():
        pytest.skip("CUDA present")
    job = SplatJob(spatial_map="x", out_dir=str(tmp_path))
    assert SplatJob.from_json(job.to_json()) == job
    with pytest.raises(GpuRequired):
        train_splat(job)
    assert not list(tmp_path.iterdir())  # nothing fabricated


@pytest.mark.heavy
def test_dataset_conventions_on_real_spatial_map():
    from formats.spatial_map import load_spatial_map

    if not REPLICA_SM.exists():
        pytest.skip("needs runs/dev_p6_replica (python -m spatial.cli run ...)")
    ds = load_dataset(load_spatial_map(REPLICA_SM), image_scale=0.5)
    inside = []
    for v in ds.train[:10]:
        H, W = v.image.shape[:2]
        pc = ds.init_xyz @ v.viewmat[:3, :3].T + v.viewmat[:3, 3]
        front = pc[:, 2] > 0.1
        uv = (pc[front] @ v.K.T)[:, :2] / pc[front][:, 2:3]
        inside.append(
            float(((uv[:, 0] >= 0) & (uv[:, 0] < W) & (uv[:, 1] >= 0) & (uv[:, 1] < H)).mean())
        )
    # a mis-scaled K or inverted pose would project almost nothing inside the image
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "phase9_dataset_replica.json").write_text(
        json.dumps(
            {
                "n_train": len(ds.train),
                "n_heldout": len(ds.heldout),
                "n_init": len(ds.init_xyz),
                "frac_inside_image": inside,
            },
            indent=1,
        )
    )
    assert len(ds.heldout) >= 5 and min(inside) > 0.2 and np.mean(inside) > 0.4


ETH3D_SM = ROOT / "runs/dev_p10_eth3d/spatial_map"


@pytest.mark.gpu(vram_gb=4)
def test_splat_training_eth3d_half_res(tmp_path):  # PENDING: GPU machine
    if not ETH3D_SM.exists():
        pytest.skip("needs runs/dev_p10_eth3d/spatial_map")
    m = train_splat(SplatJob(str(ETH3D_SM), str(tmp_path), steps=7000, image_scale=0.5))
    (RESULTS / "phase9_splat_eth3d.json").write_text(json.dumps(m, indent=1))
    assert m["psnr"] >= 24 and m["ssim"] >= 0.80 and m["lpips"] <= 0.30
