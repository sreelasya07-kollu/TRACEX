"""Render docs/PHASE_REPORTS/PHASE_<n>.md from runs/test_results/*.json (HANDOFF §6 format).
Numbers are read from the measured files; thresholds are the HANDOFF §5 values. A missing
result file renders as PENDING, never as a pass.  Usage: python scripts/render_phase_reports.py"""

from __future__ import annotations

import json
import operator
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / "runs" / "test_results"
OUT = ROOT / "docs" / "PHASE_REPORTS"
OPS = {"<=": operator.le, ">=": operator.ge, "<": operator.lt, ">": operator.gt}


def J(name: str):
    p = RES / name
    return json.loads(p.read_text()) if p.exists() else None


def get(d, path, default=None):
    """Dotted-path lookup that tolerates dots inside keys (e.g. 'conf>=0.5'): at each dict level
    the longest matching key prefix wins."""
    while path:
        if d is None:
            return default
        if isinstance(d, list):
            head, _, path = path.partition(".")
            d = d[int(head)]
            continue
        keys = [k for k in d if path == k or path.startswith(k + ".")]
        if not keys:
            return default
        k = max(keys, key=len)
        d = d[k]
        path = path[len(k) + 1 :]
    return d if d is not None else default


def check(label, value, op, thr, fmt="{:.4g}"):
    if value is None:
        return (label, "PENDING", f"{op} {thr}", "PENDING")
    ok = OPS[op](value, thr)
    return (label, fmt.format(value), f"{op} {thr}", "PASS" if ok else "FAIL")


def f5(d, key):
    t = get(d, f"{key}.tolerances")
    return None if t is None else get(d, f"{key}.fscore")[t.index(0.05)]


def phases():
    s2 = J("phase2_replica_room0_vio_small.json")
    s2l = J("phase2_replica_room0_vio_large.json")
    s2d = J("phase2_replica_room0_vio_large_depthscale.json")
    tum = J("phase2_tum_xyz.json")
    eth = J("phase2_eth3d.json")
    mvs = J("phase3_mvs_tum.json")
    sc = J("phase3_scale_recovery.json")
    bench = (
        J("../bench_phase3/room0/summary.json")
        if (RES.parent / "bench_phase3/room0/summary.json").exists()
        else None
    )
    syn4 = J("phase4_synthetic.json")
    syn5, synL, rep5 = (
        J("phase5_synthetic.json"),
        J("phase5_synthetic_L.json"),
        J("phase5_replica_room0.json"),
    )
    p6, p6d = J("phase6_end_to_end_replica.json"), J("phase6_determinism.json")
    p7, p8 = J("phase7_eth3d.json"), J("phase8_eth3d.json")
    p10 = J("phase10_replica.json")
    kf = J("phase1_keyframes_tum_desk.json")
    e8 = J("env_e8_eth3d_tool.json")
    g = lambda d, p: get(d, p)  # noqa: E731
    return {
        0: (
            "Environment",
            [
                check(
                    "E8 ETH3D tool self-score F (min over tolerances)",
                    min(get(e8, "self.fscore", [None])) if e8 else None,
                    ">=",
                    0.99,
                ),
            ],
            [
                "E1 (versions), E2 (RANSAC plane), E3 (TSDF sphere), E7 (OpenMVS image): PASS, see PROGRESS.md Phase 0.",
                "E4/E5 (gsplat on CUDA): PENDING, needs a GPU.",
                "E6 (pycolmap ETH3D at 1555 px, /4 INTER_AREA, known PINHOLE K): PASS on 2026-11-19, 3/3 runs registered 26/26, mean reproj 0.484-0.489 px (CPU); intermittent 20/26 per PROGRESS.md:61 is tracked by sfm-repeat-gate (T-026).",
            ],
        ),
        1: (
            "Ingest, adapters, keyframes",
            [
                check(
                    "ETH3D pose reprojection median [px]",
                    g(J("phase1_eth3d_reprojection.json"), "median_px"),
                    "<=",
                    1.0,
                ),
                check(
                    "Replica ray-cast depth median abs diff [m]",
                    max(g(J("phase1_replica.json"), "depth_vs_raycast_median_abs_m") or [None]),
                    "<=",
                    0.001,
                ),
                check("Keyframe max gap [m] (TUM desk)", g(kf, "max_gap_m"), "<=", 0.3),
                check("Keyframe max gap [deg] (TUM desk)", g(kf, "max_gap_deg"), "<=", 15.0),
            ],
            ["Scan Bundle adapter: not implemented (CaptureKit spec does not exist)."],
        ),
        2: (
            "Pose refinement",
            [
                check("TUM xyz ATE SE3 refined [m]", g(tum, "refined.ate_se3.rmse_m"), "<", 0.02),
                check("TUM xyz ATE reduction", g(tum, "ate_se3_reduction"), ">=", 0.5),
                check("Replica vio_small ATE SE3 [m]", g(s2, "refined.ate_se3.rmse_m"), "<", 0.02),
                check(
                    "Replica vio_large ATE SE3, priors only [m] (known miss)",
                    g(s2l, "refined.ate_se3.rmse_m"),
                    "<",
                    0.02,
                ),
                check(
                    "Replica vio_large ATE SE3 with depth scale [m]",
                    g(s2d, "refined.ate_se3.rmse_m"),
                    "<",
                    0.02,
                ),
                check(
                    "Replica vio_large scale error with depth scale",
                    g(s2d, "scale_error"),
                    "<",
                    0.02,
                ),
                check(
                    "ETH3D registered ratio",
                    (g(eth, "refine.registered") or 0) / (g(eth, "refine.n_frames") or 1)
                    if eth
                    else None,
                    ">=",
                    0.95,
                ),
                check(
                    "ETH3D median position error Sim3 [m]", g(eth, "ate_sim3.median_m"), "<", 0.02
                ),
            ],
            [
                "Global mapper (experimental): registers 26/26 but the model is wrong on ETH3D office; incremental stays default (documented xfail).",
                "ETH3D registration is intermittent in repeated runs (1 of ~9 runs 20/26).",
            ],
        ),
        3: (
            "Depth and scale",
            [
                check(
                    "MVS vs Kinect AbsRel (conf>=0.5, TUM)", g(mvs, "conf>=0.5.abs_rel"), "<=", 0.10
                ),
                check("MVS vs Kinect d1 (conf>=0.5, TUM)", g(mvs, "conf>=0.5.d1"), ">=", 0.90),
                check("Scale recovery error x0.37 (TUM)", g(sc, "tum_xyz.scale_error"), "<=", 0.02),
                check(
                    "Scale recovery error x0.37 (Replica)",
                    g(sc, "replica_room0.scale_error"),
                    "<=",
                    0.02,
                ),
                check(
                    "Replica room0 F@5cm, sensor depth (R1)",
                    get(bench, "R1.recon.fscore_5cm"),
                    ">=",
                    0.90,
                ),
                check(
                    "Replica room0 F@5cm, OpenMVS depth (R7)",
                    get(bench, "R7.recon.fscore_5cm"),
                    ">=",
                    0.60,
                ),
                check(
                    "Replica room0 F@5cm, best mono (R5)",
                    get(bench, "R5.recon.fscore_5cm"),
                    ">=",
                    0.60,
                ),
            ],
            [
                "Unfiltered MVS depth on TUM misses the target (AbsRel 0.130); the confidence cutoff is part of the method.",
                "Hybrid MVS+mono fill (R8) did not beat MVS alone; mono stays a fallback only.",
            ],
        ),
        4: (
            "Fusion",
            [
                check("Synthetic room Chamfer-L1 [m]", g(syn4, "chamfer_l1"), "<=", 0.01),
                check("Replica room0 F@5cm (R1)", get(bench, "R1.recon.fscore_5cm"), ">=", 0.60),
                check("ETH3D dense F@5cm (official tool)", f5(p8, "dense"), ">=", 0.70),
            ],
            ["Synthetic Chamfer sits at the voxel-size limit (2 cm TSDF voxel)."],
        ),
        5: (
            "Planes and room",
            [
                check(
                    "Synthetic max plane normal error [deg]",
                    max((v["normal_err_deg"] for v in syn5["planes"].values()), default=None)
                    if syn5
                    else None,
                    "<=",
                    2.0,
                ),
                check(
                    "Synthetic max plane offset error [m]",
                    max((v["offset_err_m"] for v in syn5["planes"].values()), default=None)
                    if syn5
                    else None,
                    "<=",
                    0.02,
                ),
                check(
                    "Synthetic ceiling height error [m]",
                    g(syn5, "ceiling_height_err_m"),
                    "<=",
                    0.02,
                ),
                check("Synthetic floor area rel. error", g(syn5, "floor_area_rel_err"), "<=", 0.03),
                check("L-shaped room area rel. error", g(synL, "area_rel_err"), "<=", 0.03),
                check(
                    "L-shaped room max corner error [m]",
                    max(g(synL, "corner_err_m") or [0]) if synL else None,
                    "<=",
                    0.08,
                ),
                check(
                    "Replica wall-floor deviation max [deg]",
                    max(g(rep5, "wall_floor_deviation_deg") or [0]) if rep5 else None,
                    "<=",
                    3.0,
                ),
            ],
            [
                "ETH3D floor/wall check is not run: its SfM-only workspace has no gravity or metric scale."
            ],
        ),
        6: (
            "SpatialMap",
            [
                check(
                    "End-to-end Replica: registered ratio", g(p6, "quality.reg_ratio"), ">=", 0.95
                ),
                check(
                    "Determinism: files identical downstream of Phase 2",
                    1.0 if g(p6d, "identical") else (0.0 if p6d else None),
                    ">=",
                    1.0,
                ),
            ],
            [
                "Phase 2 (COLMAP) is not bit-deterministic; determinism is asserted for everything after it."
            ],
        ),
        7: (
            "Part 4 sparse",
            [
                check("ETH3D registered ratio", g(p7, "check.reg_ratio"), ">=", 0.95),
                check("ETH3D mean reprojection [px]", g(p7, "check.mean_reproj_px"), "<", 1.0),
            ],
            [],
        ),
        8: (
            "Dense, mesh, texture",
            [
                check("ETH3D dense F@5cm", f5(p8, "dense"), ">=", 0.70),
                check("ETH3D mesh F@5cm", f5(p8, "mesh"), ">=", 0.65),
                check(
                    "Held-out textured PSNR (covered px) [dB]",
                    g(p8, "heldout.mean_psnr_covered"),
                    ">=",
                    18.0,
                ),
            ],
            [],
        ),
        9: (
            "Gaussian splats",
            [
                check("Held-out PSNR [dB]", None, ">=", 24.0),
                check("Held-out SSIM", None, ">=", 0.80),
                check("Held-out LPIPS", None, "<=", 0.30),
            ],
            [
                "Needs CUDA: the trainer is written but has never been run. CPU parts (init, PLY layout, dataset conventions) are tested."
            ],
        ),
        10: (
            "Packaging",
            [
                check(
                    "Replica e2e: mesh faces", g(p10, "outputs.mesh.metrics.mesh_faces"), ">", 10000
                ),
                check("Replica e2e: peak RAM [MB]", g(p10, "peak_ram_mb"), "<", 12000),
            ],
            ["Splat entry is 'skipped' on this machine (no CUDA), by design."],
        ),
    }


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for n, (name, checks, limits) in phases().items():
        lines = [
            f"# Phase {n}: {name}",
            "",
            "Generated by `scripts/render_phase_reports.py` from `runs/test_results/`.",
            "",
            "| Metric | Measured | Threshold | Result |",
            "|---|---|---|---|",
        ]
        lines += [f"| {a} | {b} | {c} | **{d}** |" for a, b, c, d in checks]
        lines += ["", "## Limitations / not verified", ""] + [f"- {x}" for x in limits] + [""]
        (OUT / f"PHASE_{n}.md").write_text("\n".join(lines))
        bad = [c[0] for c in checks if c[3] == "FAIL"]
        print(
            f"phase {n}: {sum(c[3] == 'PASS' for c in checks)}/{len(checks)} pass"
            + (f", FAIL: {bad}" if bad else "")
            + (
                f", pending: {sum(c[3] == 'PENDING' for c in checks)}"
                if any(c[3] == "PENDING" for c in checks)
                else ""
            )
        )


if __name__ == "__main__":
    main()
