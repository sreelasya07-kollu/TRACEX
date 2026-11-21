"""Phase 3 real-data benchmarks (Phase 3 brief §6-§8). Targets are the brief's §7 values.

Each test runs (or reuses cached) benchmark runs, writes measured values to
runs/test_results/phase3_*.json and asserts the target. Nothing here is loosened to pass.
Pending for the GPU/ARKitScenes test machine: R6 on ARKitScenes (adapter not implemented),
the CUDA parity test below, and the 300-frame / 8 GB RAM check (needs the full-length scene).
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from benchmarks.phase3 import SCENES, run

RESULTS = Path(__file__).resolve().parents[1] / "runs" / "test_results"


def _save(name, values):
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / f"phase3_{name}.json").write_text(json.dumps(values, indent=1, default=str))


def _need(scene):
    src = SCENES[scene]["src"]
    marker = src / "SUBSET.json" if SCENES[scene]["dataset"] == "replica" else src
    if not marker.exists():
        pytest.skip(f"dataset missing: {marker} (data/fetch.sh)")


def _f(ev, k="fscore_5cm", key="recon"):
    return (ev.get(key) or {}).get(k)


@pytest.mark.heavy
def test_step_b_oracle_replica():
    _need("room0")
    ev = run("room0", ["R0"])["R0"]
    _save(
        "oracle_room0",
        {
            k: ev[k]
            for k in ("recon", "recon_absolute", "n_gt_culled", "wall_time_s", "peak_ram_mb")
        },
    )
    assert _f(ev) >= 0.95  # brief §7: R0 oracle F-score@5cm >= 0.95


@pytest.mark.heavy
@pytest.mark.parametrize("scene", ["room0", "office0"])
def test_step_c_ablation(scene):
    _need(scene)
    s = run(scene, ["R0", "R1", "R2", "R3", "R4", "R5", "R6"])
    _save(
        f"ablation_{scene}",
        {
            k: {
                "recon": v.get("recon"),
                "frames_ok": v["frames_ok"],
                "valid_fraction": v["valid_fraction"],
            }
            for k, v in s.items()
        },
    )
    f = {k: _f(v) for k, v in s.items()}
    assert f["R3"] - f["R2"] >= 0.15, f  # sparse alignment is the key component
    assert f["R5"] - f["R2"] >= 0.15  # brief §7: full mono beats naive by >= 0.15
    assert f["R6"] > f["R5"]  # sensor depth beats monocular on the same pipeline


@pytest.mark.heavy
def test_step_d_depth_quality_tum_desk():
    _need("tum_desk")
    s = run("tum_desk", ["R3", "R5"])
    ev = s["R5"]
    d = ev["depth_vs_sensor_valid"]
    _save(
        "depth_quality_tum_desk",
        {
            "R5_valid": d,
            "R3_all": s["R3"]["depth_vs_sensor_all"],
            "scale_cv_raw": ev["scale_cv_raw"],
            "scale_cv_smoothed": ev["scale_cv_smoothed"],
            "n_frames": d["n_images"],
        },
    )
    assert d["n_images"] >= 50
    assert d["d1"] >= 0.85  # brief §7: aligned mono vs sensor d1 >= 0.85
    assert ev["scale_cv_smoothed"] <= ev["scale_cv_raw"]  # temporal smoothing must help


@pytest.mark.heavy
def test_step_f_robustness_room0():
    _need("room0")
    base = _f(run("room0", ["R6"])["R6"])
    out = {"baseline_R6_f5": base}
    for nm, nz in {"noise_1cm_0.5deg": [0.01, 0.5], "noise_3cm_1.5deg": [0.03, 1.5]}.items():
        out[nm] = _f(run("room0", ["R6"], {"pose_noise": nz}, f"_{nm}")[f"R6_{nm}"])
    for nm, n in {"every2": 50, "every4": 25}.items():
        ev = run("room0", ["R6"], {"max_frames": n}, f"_{nm}")[f"R6_{nm}"]
        out[nm] = {"f5": _f(ev), "completion_cm": (ev["recon"]["completion"]) * 100}
    _save("robustness_room0", out)
    # Report-only drops; assert only the physically required ordering (more noise hurts more).
    assert out["noise_3cm_1.5deg"] <= out["noise_1cm_0.5deg"] <= base + 1e-6


@pytest.mark.heavy
def test_step_g_repeatability_room0():
    _need("room0")
    f = [_f(run("room0", ["R5"], {}, f"_rep{i}")[f"R5_rep{i}"]) for i in range(3)]
    _save("repeatability_room0", {"f5_runs": f})
    assert max(f) - min(f) < 1e-6  # same seed, same cached predictions -> identical result


@pytest.mark.gpu(vram_gb=4)
def test_cuda_fusion_matches_cpu():
    """Brief §8: CUDA fusion + GPU inference parity with CPU (F-score difference < 0.01)."""
    _need("room0")
    cpu = run("room0", ["R5"], {}, "_cpu")["R5_cpu"]
    gpu = run(
        "room0",
        ["R5"],
        {"device": "cuda", "tsdf_p": {"backend": "tensor", "device": "CUDA:0"}},
        "_cuda",
    )["R5_cuda"]
    _save("gpu_parity_room0", {"cpu": cpu["recon"], "cuda": gpu["recon"]})
    assert abs(_f(cpu) - _f(gpu)) < 0.01
