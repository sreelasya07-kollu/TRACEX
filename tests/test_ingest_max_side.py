"""Opt-in `max_side` pre-resize in ingest: INTER_AREA + pixel-centre K rule."""

from __future__ import annotations

from pathlib import Path

import cv2
import numpy as np
import pytest

from formats.models import Priors
from spatial.io import adapters
from spatial.io.adapters import SourceDataset, SourceFrame
from spatial.io.ingest import ingest

pytestmark = pytest.mark.light

W, H = 200, 120
K0 = np.array([[150.0, 0, 99.5], [0, 140.0, 59.5], [0, 0, 1]])


def _loader(root: Path) -> SourceDataset:
    img = np.zeros((H, W, 3), np.uint8)
    img[60:70, 100:110] = 255  # bright 10x10 square: centre at pixel (104.5, 64.5)
    cv2.imwrite(str(root / "0.png"), img)
    frames = [SourceFrame(0, 0, root / "0.png", None, np.eye(4))]
    return SourceDataset(
        name="fake",
        scene="s",
        root=root,
        width=W,
        height=H,
        K=K0.copy(),
        dist=None,
        depth_scale_m=None,
        depth_source="none",
        frames=frames,
        has_gt=True,
        gravity_world_gt=None,
        device_pose_source="none",
    )


@pytest.fixture
def fake(monkeypatch, tmp_path):
    monkeypatch.setitem(adapters.LOADERS, "fake", _loader)
    src = tmp_path / "src"
    src.mkdir()
    return src


def _run(fake, out, **kw):
    ws = ingest("fake", fake, out, prior="gt", **kw)
    pri = Priors.model_validate_json((ws / "priors.json").read_text())
    return ws, pri


def test_max_side_maps_known_pixel_through_k(fake, tmp_path):
    ws, pri = _run(fake, tmp_path / "a", max_side=100)
    s = 100 / 200
    assert (pri.width, pri.height) == (100, 60)
    K = pri.intrinsics
    assert K.fx == pytest.approx(150 * s) and K.fy == pytest.approx(140 * s)
    assert K.cx == pytest.approx((99.5 + 0.5) * s - 0.5)
    assert K.cy == pytest.approx((59.5 + 0.5) * s - 0.5)
    # A source pixel (u, v) must land at ((u+.5)s-.5, (v+.5)s-.5): the square's centroid does.
    img = cv2.imread(str(ws / "images" / "000000.jpg"), cv2.IMREAD_GRAYSCALE).astype(float)
    assert img.shape == (60, 100)
    ys, xs = np.mgrid[0:60, 0:100]
    cx, cy = (xs * img).sum() / img.sum(), (ys * img).sum() / img.sum()
    assert cx == pytest.approx((104.5 + 0.5) * s - 0.5, abs=0.1)
    assert cy == pytest.approx((64.5 + 0.5) * s - 0.5, abs=0.1)


def test_max_side_non_integer_ratio_size_and_k(fake, tmp_path):
    _, pri = _run(fake, tmp_path / "a", max_side=137)
    s = 137 / 200
    assert (pri.width, pri.height) == (137, round(120 * s))
    assert pri.intrinsics.cx == pytest.approx((99.5 + 0.5) * s - 0.5)


def test_max_side_no_upscale_is_identical_to_default(fake, tmp_path):
    ws0, p0 = _run(fake, tmp_path / "a")
    ws1, p1 = _run(fake, tmp_path / "b", max_side=W)
    assert (ws0 / "images" / "000000.jpg").read_bytes() == (
        ws1 / "images" / "000000.jpg"
    ).read_bytes()
    assert p0.model_dump() == p1.model_dump()


def test_max_side_excludes_downscale(fake, tmp_path):
    with pytest.raises(ValueError, match="mutually exclusive"):
        _run(fake, tmp_path / "a", max_side=100, downscale=2)
