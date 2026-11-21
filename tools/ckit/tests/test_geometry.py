"""Phase 2: C++ geometry adapters cross-checked against independent references (numpy, OpenCV,
pycolmap reading our exported text model)."""

from pathlib import Path

import cv2
import numpy as np
import pycolmap
import pytest
from typer.testing import CliRunner

from ckit import geometry
from ckit.bundle.import_raw import import_raw
from ckit.cli import app
from ckit.convert.colmap import export_colmap, load_keyframe_poses
from ckit.rot import F_FLIP, mat_to_quat, quat_to_mat

K = {"width": 640, "height": 480, "fx": 510.0, "fy": 495.0, "cx": 318.2, "cy": 241.7}


def _rand_pose(rng):
    q = rng.normal(size=4)
    q /= np.linalg.norm(q)
    return rng.uniform(-2, 2, 3).tolist(), (q if q[0] >= 0 else -q).tolist()


def _points_in_view(rng, t, q, n=30):
    R = quat_to_mat(q)
    z = rng.uniform(0.5, 6, n)
    pc = np.stack([rng.uniform(-0.4, 0.4, n) * z, rng.uniform(-0.4, 0.4, n) * z, z], 1)
    return (pc @ R.T + np.asarray(t)).tolist()


@pytest.fixture(scope="module")
def bundle(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("geo") / "bundle"
    import_raw(Path(__file__).parent / "fixtures" / "raw_small", out)
    return out


def test_from_arkit_matches_numpy_flip():
    rng = np.random.default_rng(0)
    for _ in range(20):
        t, q = _rand_pose(rng)
        t2, q2 = geometry.from_arkit(t, q)
        np.testing.assert_allclose(t2, t)
        np.testing.assert_allclose(q2, mat_to_quat(quat_to_mat(q) @ F_FLIP), atol=1e-12)


def test_from_arcore_takes_xyzw():
    rng = np.random.default_rng(1)
    t, q = _rand_pose(rng)
    a = geometry.from_arcore(t, geometry.wxyz_to_xyzw(q))
    b = geometry.from_arkit(t, q)
    np.testing.assert_allclose(a[1], b[1], atol=1e-12)
    assert geometry.xyzw_to_wxyz(geometry.wxyz_to_xyzw(q)) == q


def test_project_matches_cv2_projectpoints():
    rng = np.random.default_rng(2)
    Kd = dict(K, model="OPENCV", distortion=[0.03, -0.01, 0.001, -0.0005])
    for _ in range(10):
        t, q = _rand_pose(rng)
        pts = _points_in_view(rng, t, q)
        R_wc = quat_to_mat(q)
        rvec, _ = cv2.Rodrigues(R_wc.T)
        tvec = -R_wc.T @ np.asarray(t)
        Km = np.array([[K["fx"], 0, K["cx"]], [0, K["fy"], K["cy"]], [0, 0, 1]])
        ref, _ = cv2.projectPoints(np.array(pts), rvec, tvec, Km, np.array(Kd["distortion"]))
        ours = np.array(geometry.project(Kd, t, q, pts))
        np.testing.assert_allclose(ours, ref.reshape(-1, 2), atol=1e-6)


def test_colmap_export_read_by_pycolmap(bundle, tmp_path):
    """pycolmap parses images.txt itself: proves wxyz order, cam_from_world and the +0.5 shift."""
    info = export_colmap(bundle, tmp_path / "m")
    assert info["images"] > 0 and info["cameras"] == 1
    rec = pycolmap.Reconstruction(str(tmp_path / "m"))
    kp = {f["file"]: f for f in load_keyframe_poses(bundle)["frames"]}
    rng = np.random.default_rng(3)
    for img in list(rec.images.values())[:10]:
        f = kp[img.name]
        pts = _points_in_view(rng, f["t"], f["q"], 15)
        ours = np.array(geometry.project(f["K"], f["t"], f["q"], pts))
        theirs = np.array([img.project_point(np.array(p)) for p in pts])
        np.testing.assert_allclose(theirs, ours + 0.5, atol=1e-6)


def test_rotate_scale_crop_roundtrip():
    rng = np.random.default_rng(4)
    t, q = _rand_pose(rng)
    pts = _points_in_view(rng, t, q, 10)
    t4, q4, K4 = t, q, K
    for _ in range(4):
        t4, q4, K4 = geometry.rotate_image(t4, q4, K4, 90)
    np.testing.assert_allclose(geometry.project(K4, t4, q4, pts), geometry.project(K, t, q, pts))
    Ks = geometry.scale_intrinsics(K, 320, 240)
    a = np.array(geometry.project(K, t, q, pts))
    np.testing.assert_allclose(geometry.project(Ks, t, q, pts), 0.5 * (a + 0.5) - 0.5, atol=1e-9)
    Kc = geometry.crop_intrinsics(K, 10, 20, 300, 200)
    np.testing.assert_allclose(geometry.project(Kc, t, q, pts), a - [10, 20], atol=1e-9)


def test_interpolate_and_nearest():
    t_ns = [0, 100, 400]
    ts = [[0, 0, 0], [1, 0, 0], [2, 0, 0]]
    qs = [[1, 0, 0, 0]] * 3
    r = geometry.interpolate_at(t_ns, ts, qs, [-1, 0, 50, 250, 400, 401], 150)
    assert r[0] is None and r[3] is None and r[5] is None
    np.testing.assert_allclose(r[2][0], [0.5, 0, 0])
    assert geometry.nearest_lookup(t_ns, [90, 250, 395], 10) == [1, None, 2]
    with pytest.raises(ValueError):
        geometry.interpolate_at([0, 0], ts[:2], qs[:2], [0], 10)


def test_export_colmap_cli(bundle, tmp_path):
    res = CliRunner().invoke(app, ["export-colmap", str(bundle), str(tmp_path / "m")])
    assert res.exit_code == 0, res.output
    lines = (tmp_path / "m" / "images.txt").read_text().splitlines()
    first = next(ln for ln in lines if ln and not ln.startswith("#"))
    qw = float(first.split()[1])
    assert 0 <= qw <= 1  # canonical sign, w first
