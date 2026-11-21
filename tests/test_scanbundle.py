"""Scan Bundle v0.1: validator on valid samples (tiers A/B/C) and on the broken-bundle corpus,
adapter conventions (ARKit camera axes -> OpenCV), ingest with device priors."""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import jsonschema
import numpy as np
import pytest

from eval.bundles import BROKEN, make_broken, sample_bundle
from eval.synthetic_room import cameras
from formats.scanbundle import validate_bundle
from spatial.io.ingest import ingest
from spatial.io.scanbundle import BundleError, load_scanbundle

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def bundles(tmp_path_factory):
    d = tmp_path_factory.mktemp("sb")
    return {t: sample_bundle(d / t, t, n=24) for t in "ABC"} | {"dir": d}


def codes(issues, level):
    return {i.code for i in issues if i.level == level}


@pytest.mark.light
@pytest.mark.parametrize("tier", ["A", "B", "C"])
def test_valid_samples_have_no_errors_and_match_schema(bundles, tier):
    issues, man = validate_bundle(bundles[tier])
    assert codes(issues, "error") == set(), [str(i) for i in issues]
    assert man.tier == tier and len(man.frames) == 24
    jsonschema.validate(
        json.loads((bundles[tier] / "manifest.json").read_text()),
        json.loads((ROOT / "spec/scanbundle/manifest.schema.json").read_text()),
    )
    assert (tier == "A") == all(f.depth for f in man.frames)
    assert (tier == "C") == all(f.T_world_camera is None for f in man.frames)


@pytest.mark.light
@pytest.mark.parametrize("name", sorted(BROKEN))
def test_broken_bundle_raises_expected_codes(bundles, tmp_path, name):
    broken = make_broken(bundles["A"], tmp_path)[name]
    mut, want_err, want_warn = BROKEN[name]
    issues, _ = validate_bundle(broken)
    assert want_err <= codes(issues, "error"), (name, [str(i) for i in issues])
    assert want_warn <= codes(issues, "warning"), (name, [str(i) for i in issues])
    if want_err:
        with pytest.raises(BundleError):  # the adapter never loads an invalid bundle
            load_scanbundle(broken)
    else:
        assert not codes(issues, "error")  # warning-only variants still load
        load_scanbundle(broken)


@pytest.mark.light
def test_adapter_converts_arkit_axes_to_opencv_exactly(bundles):
    ds = load_scanbundle(bundles["A"])
    gt = cameras(0)[:24]  # OpenCV-axes poses the bundle was written from
    for f, T in zip(ds.frames, gt, strict=True):
        np.testing.assert_allclose(f.T_world_camera_device, T, atol=1e-12)
    assert not ds.has_gt and ds.device_pose_source == "arkit" and ds.depth_source == "lidar"
    np.testing.assert_allclose(ds.gravity_world_gt, [0, 0, -1])
    c = load_scanbundle(bundles["C"])
    assert all(f.T_world_camera_device is None for f in c.frames) and c.depth_source == "none"


@pytest.mark.light
def test_ingest_uses_device_poses_as_priors_and_writes_no_ground_truth(bundles, tmp_path):
    ws = ingest("scanbundle", bundles["A"], tmp_path, prior="device")
    pri = json.loads((ws / "priors.json").read_text())
    assert pri["pose_source"] == "arkit" and pri["gravity_world"] == [0.0, 0.0, -1.0]
    assert pri["depth_source"] == "lidar" and not (ws / "gt").exists()
    T = np.asarray(pri["frames"][7]["T_world_camera"])
    np.testing.assert_allclose(T, cameras(0)[7], atol=1e-9)
    import cv2

    d = cv2.imread(str(ws / "depth" / "000007.png"), cv2.IMREAD_UNCHANGED)
    src = cv2.imread(str(bundles["A"] / "depth" / "000007.png"), cv2.IMREAD_UNCHANGED)
    np.testing.assert_array_equal(d, src)  # depth passes through as uint16 millimetres
    # tier C: RGB only -> prior none, no pose/gravity, no depth
    wsc = ingest("scanbundle", bundles["C"], tmp_path / "c", prior="none")
    pc = json.loads((wsc / "priors.json").read_text())
    assert pc["pose_source"] == "none" and pc["gravity_world"] is None


@pytest.mark.light
def test_ingest_refuses_ground_truth_priors_for_bundles(bundles, tmp_path):
    with pytest.raises(ValueError, match="no ground truth"):
        ingest("scanbundle", bundles["A"], tmp_path, prior="gt_noisy")
    with pytest.raises(ValueError, match="no device poses"):
        ingest("scanbundle", bundles["C"], tmp_path / "x", prior="device")


@pytest.mark.light
def test_checksum_verification_can_be_skipped_but_not_by_default(bundles, tmp_path):
    d = tmp_path / "b"
    shutil.copytree(bundles["B"], d)
    with open(d / "images" / "000001.jpg", "ab") as f:
        f.write(b"zzz")
    assert "E_CHECKSUM" in codes(validate_bundle(d)[0], "error")
    assert "E_CHECKSUM" not in codes(validate_bundle(d, check_checksums=False)[0], "error")


@pytest.mark.light
def test_validate_bundle_cli_exit_codes(bundles, tmp_path):
    from typer.testing import CliRunner

    from spatial.cli import app

    r = CliRunner().invoke(app, ["validate-bundle", "--bundle", str(bundles["A"])])
    assert r.exit_code == 0 and '"errors": 0' in r.output and '"tier": "A"' in r.output
    bad = make_broken(bundles["A"], tmp_path)["pose_teleport"]
    r = CliRunner().invoke(app, ["validate-bundle", "--bundle", str(bad)])
    assert r.exit_code == 1 and "E_POSE_TELEPORT" in r.output
