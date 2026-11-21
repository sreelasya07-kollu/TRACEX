import json

import numpy as np
from typer.testing import CliRunner

from ckit.cli import app
from ckit.inspect_raw import plot, summarize
from ckit.raw import RawSession, rate_stats
from ckit.rot import look_rotation, mat_to_quat, quat_to_mat
from ckit.synth.render import Cam
from ckit.synth.scenarios import generate


def test_rate_stats_counts_drops() -> None:
    t = np.arange(0, 1_000_000_000, 5_000_000, dtype=np.int64)
    t = np.delete(t, [10, 11, 50])
    s = rate_stats(t, nominal_hz=200)
    assert s["dropped"] == 3 and s["non_monotonic"] == 0 and s["hz"] == 200.0


def test_inspect_raw_fixture(raw_small, tmp_path) -> None:
    s = RawSession.load(raw_small)
    out = summarize(s)
    assert out["poses"]["hz"] == 30.0 and out["gyro"]["hz"] == 200.0
    assert out["frames"]["count"] == len(s.frames) > 0
    assert plot(s, tmp_path / "p.png").stat().st_size > 1000
    r = CliRunner().invoke(app, ["inspect-raw", str(raw_small)])
    assert r.exit_code == 0 and json.loads(r.stdout)["session"] == "raw_small"


def test_quaternion_roundtrip() -> None:
    R = look_rotation(0.7, -0.3, 0.2)
    q = mat_to_quat(R)
    assert q[0] >= 0 and abs(np.linalg.norm(q) - 1) < 1e-12
    np.testing.assert_allclose(quat_to_mat(q), R, atol=1e-12)


def test_look_rotation_is_opencv() -> None:
    R = look_rotation(0.0, 0.0)  # looking along +Z world
    np.testing.assert_allclose(R[:, 2], [0, 0, 1], atol=1e-12)  # optical axis
    np.testing.assert_allclose(R[:, 1], [0, -1, 0], atol=1e-12)  # image down = world down


def test_synth_is_deterministic(tmp_path) -> None:
    cam = Cam(w=64, h=48, fx=50, fy=50, cx=31.5, cy=23.5)
    a = generate("G1", tmp_path / "a", seed=5, duration=1.0, frame_hz=2, cam=cam)
    b = generate("G1", tmp_path / "b", seed=5, duration=1.0, frame_hz=2, cam=cam)
    for f in ("poses.jsonl", "imu.jsonl", "frames.jsonl", "frames/000000.jpg"):
        assert (a / f).read_bytes() == (b / f).read_bytes(), f
