"""Phase 3 Step A: evaluator known-answer checks (exact or within stated tolerance)."""

import numpy as np
import open3d as o3d
import pytest

from eval.depth_metrics import depth_metrics
from eval.recon_metrics import cull_to_cameras, recon_metrics


def _plane(n=200_000, seed=0):
    """Dense 2x2 m plane z=0 (normals +Z)."""
    rng = np.random.default_rng(seed)
    p = np.c_[rng.uniform(-1, 1, (n, 2)), np.zeros(n)]
    return p, np.tile([0, 0, 1.0], (n, 1))


def _box_room(seed=0):
    m = o3d.geometry.TriangleMesh.create_box(4.0, 3.0, 2.5).translate((-2, -1.5, 0))
    m.compute_triangle_normals()
    o3d.utility.random.seed(seed)
    pc = m.sample_points_uniformly(300_000, use_triangle_normal=True)
    return m, np.asarray(pc.points), np.asarray(pc.normals)


@pytest.mark.light
def test_gt_vs_itself():
    _, p, n = _box_room()
    m = recon_metrics(p, p, n, n)
    assert m["chamfer_l1"] == 0.0 and m["fscore_2cm"] == 1.0 and m["fscore_5cm"] == 1.0
    assert m["normal_consistency"] == pytest.approx(1.0)


@pytest.mark.light
def test_translated_1cm_along_normal():
    """Shift along the surface normal => every NN distance is exactly 1 cm."""
    p, n = _plane()
    m = recon_metrics(p + [0, 0, 0.01], p, n, n)
    assert m["accuracy"] == pytest.approx(0.01, abs=0.001)
    assert m["completion"] == pytest.approx(0.01, abs=0.001)
    assert m["fscore_2cm"] == 1.0  # all within 2 cm


@pytest.mark.light
def test_spatial_half_subset():
    """Keep one spatially contiguous half: precision ~1, recall ~0.5 (clearly < 1)."""
    _, p, _ = _box_room()
    half = p[p[:, 0] < 0]
    m = recon_metrics(half, p)
    assert m["precision_5cm"] > 0.999
    assert 0.4 < m["recall_5cm"] < 0.6


@pytest.mark.light
def test_random_half_subset_documents_why_spatial_is_needed():
    """A *random* 50 % subset of a dense cloud keeps recall near 1 (measured 0.980 @2 cm with
    300k samples on a 4x3x2.5 m box): dropped points keep neighbours within mm. Hence the
    brief's "random subset -> recall clearly < 1" can't hold for dense GT; the spatial-half
    test above checks the intended property."""
    _, p, _ = _box_room()
    rng = np.random.default_rng(0)
    m = recon_metrics(p[rng.random(len(p)) < 0.5], p)
    assert m["precision_2cm"] > 0.999 and m["recall_2cm"] > 0.95


@pytest.mark.light
def test_depth_known_answers():
    rng = np.random.default_rng(0)
    gt = rng.uniform(0.5, 8.0, (480, 640))
    a = depth_metrics(gt, gt)
    assert a["abs_rel"] == 0 and a["rmse"] == 0 and a["d1"] == 1.0
    b = depth_metrics(gt * 1.1, gt)
    assert b["abs_rel"] == pytest.approx(0.1, abs=1e-12)
    assert b["d1"] == 1.0  # ratio 1.1 < 1.25
    c = depth_metrics(gt * 1.3, gt)
    assert c["d1"] == 0.0 and c["d2"] == 1.0


@pytest.mark.light
def test_culling_frustum_and_occlusion():
    """Camera inside a box room looking at +X wall: only that wall (and nothing behind it)
    survives; a box in front of the wall hides the wall region behind it."""
    room, p, _ = _box_room()
    obstacle = o3d.geometry.TriangleMesh.create_box(0.2, 0.6, 0.6).translate((1.0, -0.3, 0.95))
    scene_mesh = room + obstacle
    T_wc = np.eye(4)
    T_wc[:3, :3] = np.array([[0, 0, 1], [-1, 0, 0], [0, -1, 0]])  # camera z -> world +x
    T_wc[:3, 3] = [0, 0, 1.25]
    # f=400 px: half-FOV 21.8 deg (h) / 16.7 deg (v) -> only the far wall is in view.
    K = np.array([[400.0, 0, 159.5], [0, 400.0, 119.5], [0, 0, 1]])
    keep = cull_to_cameras(p, [(T_wc, K, 320, 240)], gt_mesh=scene_mesh)
    kept = p[keep]
    assert np.all(kept[:, 0] > 1.99)  # only the +X wall is visible
    # Wall points straight behind the obstacle (y,z within its silhouette core) are hidden.
    behind = (np.abs(p[:, 1]) < 0.1) & (np.abs(p[:, 2] - 1.25) < 0.1) & (p[:, 0] > 1.99)
    assert behind.sum() > 0 and not keep[behind].any()
