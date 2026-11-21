"""`spatial.cli ingest --dry-run`: reports the selection, writes nothing, validates as ingest."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
from typer.testing import CliRunner

from spatial.cli import app
from spatial.io import adapters
from spatial.io.adapters import SourceDataset, SourceFrame

pytestmark = pytest.mark.light


def _fake_loader(*, has_gt: bool = True, device: str = "none", n: int = 10, gravity: bool = False):
    def load(root: Path) -> SourceDataset:
        frames = [
            SourceFrame(i, i * 1000, root / f"{i}.jpg", None, np.eye(4) if has_gt else None)
            for i in range(n)
        ]
        return SourceDataset(
            name="fake",
            scene="s",
            root=root,
            width=64,
            height=48,
            K=np.eye(3),
            dist=None,
            depth_scale_m=None,
            depth_source="none",
            frames=frames,
            has_gt=has_gt,
            gravity_world_gt=np.array([0.0, 1.0, 0.0]) if gravity else None,
            device_pose_source=device,
        )

    return load


def _run(tmp_path, monkeypatch, loader, *args):
    monkeypatch.setitem(adapters.LOADERS, "fake", loader)
    out = tmp_path / "X"
    base = ["ingest", "--dataset", "fake", "--src", str(tmp_path), "--out", str(out), "--dry-run"]
    res = CliRunner().invoke(app, [*base, *args])
    return res, out


def test_dry_run_reports_and_writes_nothing(tmp_path, monkeypatch):
    res, out = _run(
        tmp_path, monkeypatch, _fake_loader(), "--stride", "2", "--max-frames", "3", "--prior", "gt"
    )
    assert res.exit_code == 0, res.output
    info = json.loads(res.output)
    assert info["dry_run"] is True and info["frames"] == 3 and info["prior"] == "gt"
    assert info["frames_available"] == 10 and info["dataset"] == "fake"
    assert not out.exists()


def test_dry_run_same_validation_as_ingest(tmp_path, monkeypatch):
    res, out = _run(tmp_path, monkeypatch, _fake_loader(has_gt=False), "--prior", "gt_noisy")
    assert isinstance(res.exception, ValueError) and "no ground truth" in str(res.exception)
    res, _ = _run(tmp_path, monkeypatch, _fake_loader(), "--prior", "device")
    assert isinstance(res.exception, ValueError) and "no device poses" in str(res.exception)
    res, _ = _run(tmp_path, monkeypatch, _fake_loader(n=0), "--prior", "gt")
    assert isinstance(res.exception, ValueError) and "no frames selected" in str(res.exception)
    assert not out.exists()


def test_dry_run_device_prior_ok(tmp_path, monkeypatch):
    res, _ = _run(
        tmp_path, monkeypatch, _fake_loader(has_gt=False, device="arkit"), "--prior", "device"
    )
    assert res.exit_code == 0 and json.loads(res.output)["frames"] == 10


def test_dry_run_gt_noisy_needs_gravity(tmp_path, monkeypatch):
    res, out = _run(tmp_path, monkeypatch, _fake_loader(), "--prior", "gt_noisy")
    assert isinstance(res.exception, ValueError) and "needs a gravity" in str(res.exception)
    assert not out.exists()
    res, _ = _run(tmp_path, monkeypatch, _fake_loader(gravity=True), "--prior", "gt_noisy")
    assert res.exit_code == 0 and json.loads(res.output)["prior"] == "gt_noisy"


def test_help_lists_dry_run():
    assert "--dry-run" in CliRunner().invoke(app, ["ingest", "--help"]).output
