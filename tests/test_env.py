"""Environment acceptance tests (HANDOFF.md §3, Step 0). Each test measures a quantity.

GPU-only tests (E4, E5) are skipped when CUDA is unavailable; E6-E8 need fetched data / docker.
"""

import shutil
import subprocess
from pathlib import Path

import numpy as np
import open3d as o3d
import pytest
import torch

DATA = Path(__file__).resolve().parents[1] / "data"
cuda = pytest.mark.gpu(vram_gb=1)  # inventory P0-02


def test_e1_versions():
    import gsplat
    import pycolmap

    assert torch.__version__.startswith("2.14.1")
    assert gsplat.__version__ == "1.5.3"
    assert o3d.__version__ == "0.19.0"
    assert pycolmap.__version__ == "4.2.1"
    print(
        "cuda:",
        torch.cuda.is_available(),
        torch.cuda.get_device_name(0) if torch.cuda.is_available() else "-",
    )


def test_e2_ransac_plane():
    rng = np.random.default_rng(0)
    pts = np.c_[rng.uniform(-2, 2, (5000, 2)), 0.5 + rng.normal(0, 0.005, 5000)]
    pts = np.vstack([pts, rng.uniform(-2, 2, (500, 3))])
    pc = o3d.geometry.PointCloud(o3d.utility.Vector3dVector(pts))
    (a, b, c, d), inliers = pc.segment_plane(0.02, 3, 1000, probability=0.99999999)
    n = np.array([a, b, c]) * np.sign(c)
    angle = np.degrees(np.arccos(np.clip(n @ [0, 0, 1], -1, 1)))
    assert angle < 1.0
    assert abs(abs(d) - 0.5) < 0.005
    assert len(inliers) >= 4950


def _look_at(c, target):
    z = (target - c) / np.linalg.norm(target - c)
    x = np.cross([0, -1, 0], z)
    x /= np.linalg.norm(x)
    T = np.eye(4)
    T[:3, :3] = np.c_[x, np.cross(z, x), z]  # OpenCV camera: x right, y down, z forward
    T[:3, 3] = c
    return T  # T_world_camera


def test_e3_scalable_tsdf_sphere():
    centre, radius = np.array([0.0, 0.0, 2.0]), 0.5
    mesh = o3d.geometry.TriangleMesh.create_sphere(radius, 128)
    mesh.translate(centre)
    scene = o3d.t.geometry.RaycastingScene()
    scene.add_triangles(o3d.t.geometry.TriangleMesh.from_legacy(mesh))
    W, H, f = 320, 240, 256.0
    K = o3d.camera.PinholeCameraIntrinsic(W, H, f, f, W / 2, H / 2)
    vol = o3d.pipelines.integration.ScalableTSDFVolume(
        voxel_length=0.01,
        sdf_trunc=0.04,
        color_type=o3d.pipelines.integration.TSDFVolumeColorType.RGB8,
    )
    for ang in np.linspace(-np.pi, np.pi, 8, endpoint=False):
        T_wc = _look_at(centre + 2.0 * np.array([np.sin(ang), 0.0, -np.cos(ang)]), centre)
        T_cw = np.linalg.inv(T_wc)
        rays = o3d.t.geometry.RaycastingScene.create_rays_pinhole(
            o3d.core.Tensor(K.intrinsic_matrix), o3d.core.Tensor(T_cw), W, H
        )
        t = scene.cast_rays(rays)["t_hit"].numpy()
        dirs_cam = rays.numpy()[..., 3:] @ T_cw[:3, :3].T
        depth = np.where(np.isfinite(t), t * dirs_cam[..., 2], 0).astype(np.float32)
        rgbd = o3d.geometry.RGBDImage.create_from_color_and_depth(
            o3d.geometry.Image(np.zeros((H, W, 3), np.uint8)),
            o3d.geometry.Image(depth),
            depth_scale=1.0,
            depth_trunc=10.0,
            convert_rgb_to_intensity=False,
        )
        vol.integrate(rgbd, K, T_cw)
    v = np.asarray(vol.extract_triangle_mesh().vertices)
    assert len(v) > 1000
    r = np.linalg.norm(v - centre, axis=1)
    print(f"sphere radius mean={r.mean():.5f} std={r.std():.5f} verts={len(v)}")
    assert abs(r.mean() - radius) < 0.005 and r.std() < 0.005


@cuda
def test_e4_gsplat_projection():
    from gsplat import rasterization

    dev = "cuda"
    W, H, f = 64, 48, 50.0
    K = torch.tensor([[f, 0, W / 2], [0, f, H / 2], [0, 0, 1]], device=dev)[None]
    means = torch.tensor([[0.1, -0.05, 2.0]], device=dev)
    img, _, _ = rasterization(
        means=means,
        quats=torch.tensor([[1.0, 0, 0, 0]], device=dev),
        scales=torch.full((1, 3), 0.01, device=dev),
        opacities=torch.ones(1, device=dev),
        colors=torch.ones(1, 3, device=dev),
        viewmats=torch.eye(4, device=dev)[None],
        Ks=K,
        width=W,
        height=H,
    )
    g = img[0, ..., 0]
    yx = np.unravel_index(int(torch.argmax(g)), g.shape)
    u, v = f * 0.1 / 2.0 + W / 2, f * -0.05 / 2.0 + H / 2
    assert abs(yx[1] + 0.5 - u) <= 1.0 and abs(yx[0] + 0.5 - v) <= 1.0


@cuda
def test_e5_gsplat_fit_image():
    from gsplat import rasterization
    from torchmetrics.image import PeakSignalNoiseRatio

    torch.manual_seed(0)
    dev, W, H, N = "cuda", 128, 128, 2000
    yy, xx = torch.meshgrid(torch.linspace(0, 1, H), torch.linspace(0, 1, W), indexing="ij")
    target = torch.stack([xx, yy, 0.5 + 0.5 * torch.sin(8 * xx) * torch.cos(8 * yy)], -1).to(dev)
    K = torch.tensor([[100.0, 0, W / 2], [0, 100.0, H / 2], [0, 0, 1]], device=dev)[None]
    means = torch.nn.Parameter(
        torch.cat(
            [(torch.rand(N, 2, device=dev) - 0.5) * 2.6, torch.full((N, 1), 2.0, device=dev)], 1
        )
    )
    scales = torch.nn.Parameter(torch.full((N, 3), -4.0, device=dev))
    quats = torch.nn.Parameter(torch.tensor([[1.0, 0, 0, 0]], device=dev).repeat(N, 1))
    opac = torch.nn.Parameter(torch.zeros(N, device=dev))
    rgb = torch.nn.Parameter(torch.zeros(N, 3, device=dev))
    opt = torch.optim.Adam([means, scales, quats, opac, rgb], lr=0.01)
    psnr = PeakSignalNoiseRatio(data_range=1.0).to(dev)
    hist = []
    for _ in range(300):
        img, _, _ = rasterization(
            means,
            quats / quats.norm(dim=-1, keepdim=True),
            scales.exp(),
            torch.sigmoid(opac),
            torch.sigmoid(rgb),
            torch.eye(4, device=dev)[None],
            K,
            W,
            H,
        )
        loss = torch.nn.functional.l1_loss(img[0], target)
        opt.zero_grad()
        loss.backward()
        opt.step()
        hist.append(
            float(psnr(img[0].clamp(0, 1).permute(2, 0, 1)[None], target.permute(2, 0, 1)[None]))
        )
    print(f"psnr start={hist[0]:.2f} end={hist[-1]:.2f}")
    assert np.mean(hist[-20:]) > np.mean(hist[:20]) and hist[-1] > 25.0


def test_e6_pycolmap_sfm_eth3d(tmp_path):
    import cv2
    import pycolmap

    root = DATA / "eth3d" / "office"
    src = root / "images" / "dslr_images_undistorted"
    cams = root / "dslr_calibration_undistorted" / "cameras.txt"
    if not src.is_dir() or not cams.is_file():
        pytest.skip("ETH3D office not fetched (data/fetch.sh eth3d)")
    names = sorted(p.name for p in src.glob("*.JPG"))[:30]
    # Same inputs as spatial.sfm.refine gets from ingest: known PINHOLE intrinsics (fixed) and
    # images pre-shrunk with INTER_AREA. COLMAP's internal resize of the 6221 px originals and
    # EXIF-less focal guessing each leave 6 images in a separate model (20/26, measured).
    D = 4
    imgs = tmp_path / "images"
    imgs.mkdir()
    for n in names:
        im = cv2.imread(str(src / n), cv2.IMREAD_COLOR)
        h, w = im.shape[:2]
        im = cv2.resize(im, (w // D, h // D), interpolation=cv2.INTER_AREA)
        cv2.imwrite(str(imgs / n), im, [cv2.IMWRITE_JPEG_QUALITY, 95])
    line = next(ln for ln in cams.read_text().splitlines() if ln.strip() and ln[0] != "#")
    fx, fy, cx, cy = map(float, line.split()[4:8])
    K = [fx / D, fy / D, (cx + 0.5) / D - 0.5, (cy + 0.5) / D - 0.5]  # pixel-centre rule
    reader = pycolmap.ImageReaderOptions()
    reader.camera_model = "PINHOLE"
    reader.camera_params = ",".join(map(str, K))
    db = tmp_path / "db.db"
    opts = pycolmap.FeatureExtractionOptions()
    opts.max_image_size = 1600  # pycolmap 4.2: on FeatureExtractionOptions, not .sift
    pycolmap.extract_features(
        db,
        imgs,
        image_names=names,
        extraction_options=opts,
        reader_options=reader,
        camera_mode=pycolmap.CameraMode.SINGLE,
    )
    pycolmap.match_exhaustive(db)
    mopts = pycolmap.IncrementalPipelineOptions()
    mopts.ba_refine_focal_length = False
    mopts.ba_refine_principal_point = False
    mopts.ba_refine_extra_params = False
    mopts.random_seed = 0
    pycolmap.set_random_seed(0)
    (tmp_path / "sparse").mkdir()
    maps = pycolmap.incremental_mapping(db, imgs, tmp_path / "sparse", options=mopts)
    best = max(maps.values(), key=lambda r: r.num_reg_images())
    reg, err = best.num_reg_images(), best.compute_mean_reprojection_error()
    print(f"registered {reg}/{len(names)}, mean reproj {err:.3f} px")
    assert reg >= 0.9 * len(names) and err < 1.5


@pytest.mark.skipif(shutil.which("docker") is None, reason="docker not installed")
def test_e7_openmvs_image():
    # OpenMVS apps exit 1 after printing --help, so check the banner + loader instead.
    for app in [
        "InterfaceCOLMAP",
        "DensifyPointCloud",
        "ReconstructMesh",
        "RefineMesh",
        "TextureMesh",
    ]:
        r = subprocess.run(
            ["docker", "run", "--rm", "room-openmvs:2.4.0", app, "--help"],
            capture_output=True,
            text=True,
        )
        out = r.stdout + r.stderr
        if "Unable to find image" in out or "No such image" in out:
            pytest.skip("room-openmvs:2.4.0 not built")
        assert "OpenMVS x64 v2.4.0" in out, out[-1000:]
        assert "error while loading shared libraries" not in out
