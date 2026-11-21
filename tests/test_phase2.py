"""Phase 2 (pose refinement) benchmarks on real data. Thresholds: HANDOFF.md §5 (PROPOSED).

TUM GT orientation carries a ~1.8 deg camera/mocap offset (measured: SfM rotations agree
with ETH3D's laser-registered GT to 0.16 deg), so TUM is judged on translation only;
rotation accuracy is judged on ETH3D and Replica.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

from eval.trajectory import ate, rpe
from spatial.io.ingest import ingest
from spatial.sfm.refine import poses_in_gt_frame, refine

DATA = Path(__file__).resolve().parents[1] / "data"
RESULTS = Path(__file__).resolve().parents[1] / "runs" / "test_results"


def _save(name, values):
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / f"{name}.json").write_text(json.dumps(values, indent=1))


def _load(ws):
    gt = {k: np.asarray(v) for k, v in json.loads((ws / "gt/poses_gt.json").read_text()).items()}
    pri = {
        f["id"]: np.asarray(f["T_world_camera"])
        for f in json.loads((ws / "priors.json").read_text())["frames"]
        if f["T_world_camera"] is not None
    }
    return gt, pri, poses_in_gt_frame(ws)


def _need(p):
    if not p.exists():
        pytest.skip(f"dataset missing: {p}")


@pytest.mark.heavy
def test_eth3d_sfm_only(tmp_path):
    root = DATA / "eth3d" / "office"
    _need(root)
    ws = ingest("eth3d", root, tmp_path, downscale=4, prior="none")
    r = refine(ws, tmp_path, max_image_size=1600, matcher="exhaustive")
    gt, _, est = _load(ws)
    a = ate(est, gt, "sim3")
    meta = json.loads((tmp_path / "phase2" / "meta.json").read_text())
    _save(
        "phase2_eth3d",
        {
            "refine": {k: r[k] for k in ("registered", "n_frames", "mean_reproj_px", "n_points3D")},
            "ate_sim3": a,
            "wall_time_s": meta["wall_time_s"],
            "peak_ram_mb": meta["peak_ram_mb"],
        },
    )
    assert r["registered"] / r["n_frames"] >= 0.95
    assert r["mean_reproj_px"] < 1.0
    assert a["median_m"] < 0.02
    assert a["rot_mean_deg"] < 0.5


@pytest.mark.heavy
def test_eth3d_sfm_repeat(tmp_path):
    """Issue #4: registration on ETH3D office is intermittent (multithreaded COLMAP is not
    bit-deterministic). Gate on the minimum over 5 seeded runs, multithreaded as in production.
    One ingested workspace and one run dir are shared: each refine call wipes its COLMAP work
    dirs, and the stage records outputs relative to the run dir, so the workspace must sit inside
    it (a separate run dir per call raises in record_stage). Metrics come from the return value."""
    root = DATA / "eth3d" / "office"
    _need(root)
    ws = ingest("eth3d", root, tmp_path, downscale=4, prior="none")
    runs = []
    for seed in range(5):
        r = refine(
            ws,
            tmp_path,
            max_image_size=1600,
            matcher="exhaustive",
            seed=seed,
            num_threads=-1,
        )
        runs.append(
            {
                "seed": seed,
                "status": r.get("status", "ok"),
                "registered": r.get("registered", 0),
                "n_frames": r.get("n_frames"),
                "mean_reproj_px": r.get("mean_reproj_px"),
                "model_sizes": r.get("model_sizes"),
            }
        )
    n = len(json.loads((ws / "priors.json").read_text())["frames"])  # frames in the workspace
    reg = [x["registered"] for x in runs]
    _save(
        "phase2_eth3d_repeat",
        {
            "runs": runs,
            "n_frames": n,
            "registered_min": min(reg),
            "registered_median": float(np.median(reg)),
            "full_runs": sum(x == n for x in reg),
        },
    )
    assert n > 0
    assert all(x["status"] != "failed" for x in runs), runs
    assert all(x["n_frames"] == n for x in runs), [x["n_frames"] for x in runs]
    assert min(reg) >= 0.95 * n, f"registered per run {reg} of {n}"


@pytest.mark.heavy
@pytest.mark.xfail(
    strict=False,
    reason="KNOWN MISS (measured 2026-11-14): pycolmap 4.2.1 global mapper registers 26/26 at "
    "0.51 px "
    "but returns a geometrically wrong model on ETH3D office (Sim(3) residual 3.6 m vs the "
    "incremental model over a 10 m extent, rotation error ~177 deg, ATE median 0.41 m). "
    "mapper='global' is experimental; 'incremental' stays the default.",
)
def test_eth3d_global_mapper(tmp_path):
    """P2-04: COLMAP 4 global mapper (GLOMAP) on the same input as test_eth3d_sfm_only. Same
    acceptance; the incremental numbers are in phase2_eth3d.json for comparison."""
    root = DATA / "eth3d" / "office"
    _need(root)
    ws = ingest("eth3d", root, tmp_path, downscale=4, prior="none")
    r = refine(ws, tmp_path, max_image_size=1600, matcher="exhaustive", mapper="global")
    gt, _, est = _load(ws)
    a = ate(est, gt, "sim3")
    meta = json.loads((tmp_path / "phase2" / "meta.json").read_text())
    _save(
        "phase2_eth3d_global",
        {
            "refine": {k: r[k] for k in ("registered", "n_frames", "mean_reproj_px", "n_points3D")},
            "ate_sim3": a,
            "wall_time_s": meta["wall_time_s"],
            "peak_ram_mb": meta["peak_ram_mb"],
        },
    )
    assert r["registered"] / r["n_frames"] >= 0.95
    assert r["mean_reproj_px"] < 1.0
    assert a["median_m"] < 0.02
    assert a["rot_mean_deg"] < 0.5


@pytest.mark.heavy
@pytest.mark.parametrize("seq", ["rgbd_dataset_freiburg1_xyz", "rgbd_dataset_freiburg1_desk"])
def test_tum_refinement_vs_noisy_prior(tmp_path, seq):
    root = DATA / "tum" / seq
    _need(root)
    ws = ingest(
        "tum",
        root,
        tmp_path,
        max_frames=60,
        stride=10,
        prior="gt_noisy",
        noise_preset="vio_large",
        seed=0,
    )
    r = refine(ws, tmp_path, max_image_size=640)
    gt, pri, est = _load(ws)
    res = {
        "seq": seq,
        "registered": r["registered"],
        "n_frames": r["n_frames"],
        "mean_reproj_px": r["mean_reproj_px"],
    }
    for name, P in [("prior", pri), ("refined", est)]:
        res[name] = {
            "ate_none": ate(P, gt, "none"),
            "ate_se3": ate(P, gt, "se3"),
            "ate_sim3": ate(P, gt, "sim3"),
            "rpe": rpe(P, gt),
        }
    res["ate_se3_reduction"] = (
        1 - res["refined"]["ate_se3"]["rmse_m"] / res["prior"]["ate_se3"]["rmse_m"]
    )
    res["scale_error"] = abs(res["refined"]["ate_sim3"]["scale"] - 1)
    meta = json.loads((tmp_path / "phase2" / "meta.json").read_text())
    res |= {"wall_time_s": meta["wall_time_s"], "peak_ram_mb": meta["peak_ram_mb"]}
    _save(f"phase2_tum_{seq.split('_')[-1]}", res)
    assert r["registered"] / r["n_frames"] >= 0.95
    assert res["ate_se3_reduction"] >= 0.5
    assert res["refined"]["ate_se3"]["rmse_m"] < 0.02
    assert res["scale_error"] < 0.02


@pytest.mark.heavy
@pytest.mark.parametrize(
    "preset,depth_scale",
    [
        ("vio_small", False),
        pytest.param(
            "vio_large",
            False,
            marks=pytest.mark.xfail(
                strict=False,
                reason="KNOWN MISS (measured 2026-11-13): refined ATE_se3 2.63 cm > 2 cm. Shape is "
                "fine (ATE_sim3 0.49 cm, rot 0.14 deg); scale inherited from 3%/m drifting priors "
                "over a 23 m path (scale error 2.35%). Fixed by depth_scale=True (next case).",
            ),
        ),
        ("vio_large", True),
    ],
)
def test_replica_refinement_vs_noisy_prior(tmp_path, preset, depth_scale):
    root = DATA / "replica" / "room0"
    _need(root / "SUBSET.json")
    ws = ingest(
        "replica",
        root,
        tmp_path,
        max_frames=80,
        stride=5,
        downscale=2,
        prior="gt_noisy",
        noise_preset=preset,
        seed=0,
    )
    r = refine(ws, tmp_path, max_image_size=640, depth_scale=depth_scale)
    gt, pri, est = _load(ws)
    res = {
        "preset": preset,
        "depth_scale": depth_scale,
        "depth_scale_info": r.get("depth_scale"),
        "registered": r["registered"],
        "n_frames": r["n_frames"],
        "mean_reproj_px": r["mean_reproj_px"],
    }
    for name, P in [("prior", pri), ("refined", est)]:
        res[name] = {
            "ate_none": ate(P, gt, "none"),
            "ate_se3": ate(P, gt, "se3"),
            "ate_sim3": ate(P, gt, "sim3"),
            "rpe": rpe(P, gt),
        }
    res["ate_se3_reduction"] = (
        1 - res["refined"]["ate_se3"]["rmse_m"] / res["prior"]["ate_se3"]["rmse_m"]
    )
    res["scale_error"] = abs(res["refined"]["ate_sim3"]["scale"] - 1)
    meta = json.loads((tmp_path / "phase2" / "meta.json").read_text())
    res |= {"wall_time_s": meta["wall_time_s"], "peak_ram_mb": meta["peak_ram_mb"]}
    _save(f"phase2_replica_room0_{preset}{'_depthscale' if depth_scale else ''}", res)
    assert r["registered"] / r["n_frames"] >= 0.95
    assert res["ate_se3_reduction"] >= 0.5
    assert res["refined"]["ate_se3"]["rot_mean_deg"] < 0.5
    assert res["refined"]["ate_se3"]["rmse_m"] < 0.02
