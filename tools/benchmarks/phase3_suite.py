"""Phase 3 suite: Steps C (ablation), D (depth quality), E (efficiency/voxel sweep),
F (robustness), G (repeatability). Every number comes from runs/bench_phase3/**/eval.json.

  python -m benchmarks.phase3_suite --steps C D E F G [--scenes room0 office0]
Writes runs/bench_phase3/tables.md and tables.json. Re-running reuses cached runs.
"""

from __future__ import annotations

import argparse
import json

import numpy as np

from benchmarks.phase3 import BENCH, run

SWEEP = [0.01, 0.02, 0.04]
NOISE = {"noise_1cm_0.5deg": [0.01, 0.5], "noise_3cm_1.5deg": [0.03, 1.5]}
SUBSAMPLE = {"every2": 50, "every4": 25}  # of the 100 selected frames
ABLATION = ["R0", "R1", "R2", "R3", "R4", "R5", "R6"]
LABEL = {
    "R0": "oracle: GT poses + sensor depth, no filters",
    "R1": "refined poses + sensor depth + masks + consistency",
    "R2": "refined poses + mono raw (no align/masks/consistency)",
    "R3": "+ sparse alignment",
    "R4": "+ masks",
    "R5": "+ consistency (full mono pipeline)",
    "R6": "= R1 (full LiDAR/sensor pipeline)",
}


def _r(ev: dict, key="recon") -> dict:
    return ev.get(key) or {}


def fmt(x, nd=3, scale=1.0):
    return "-" if x is None or (isinstance(x, float) and np.isnan(x)) else f"{x * scale:.{nd}f}"


def row_3d(name, ev) -> str:
    r = _r(ev)
    return (
        f"| {name} | {fmt(r.get('fscore_2cm'))} | {fmt(r.get('fscore_5cm'))} | "
        f"{fmt(r.get('accuracy'), 2, 100)} | {fmt(r.get('completion'), 2, 100)} | "
        f"{fmt(r.get('chamfer_l1'), 2, 100)} | {fmt(r.get('normal_consistency'))} | "
        f"{ev['n_points']} |"
    )


HEAD_3D = (
    "| run | F@2cm | F@5cm | acc cm | comp cm | chamfer cm | normal cons. | points |\n"
    "|---|---|---|---|---|---|---|---|"
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", nargs="+", default=list("CDEFG"))
    ap.add_argument("--scenes", nargs="+", default=["room0", "office0"])
    a = ap.parse_args()
    md, js = [], {}

    if "C" in a.steps:
        for sc in a.scenes:
            s = run(sc, ABLATION)
            js[f"C_{sc}"] = s
            md += [f"\n### Step C ablation: {sc}", "", HEAD_3D]
            md += [row_3d(f"{k}: {LABEL[k]}", s[k]) for k in ABLATION]
            md += [
                "",
                "Absolute (untrajectory-aligned) metrics, for reference: "
                + "; ".join(
                    f"{k} F@5={fmt(_r(s[k], 'recon_absolute').get('fscore_5cm'))}" for k in ABLATION
                ),
            ]

    if "D" in a.steps:
        s = run("tum_desk", ["R3", "R4", "R5"])
        js["D_tum_desk"] = s
        md += [
            "\n### Step D depth quality: TUM fr1/desk, aligned mono vs Kinect depth",
            "",
            "| run | frames ok/rejected | AbsRel | RMSE m | d1 | d2 | d3 | scale CV raw | "
            "scale CV smoothed |",
            "|---|---|---|---|---|---|---|---|---|",
        ]
        for k in ("R3", "R4", "R5"):
            ev = s[k]
            d = ev.get("depth_vs_sensor_valid") or ev.get("depth_vs_sensor_all") or {}
            md.append(
                f"| {k} | {ev['frames_ok']}/{ev['frames_rejected']} | {fmt(d.get('abs_rel'))} | "
                f"{fmt(d.get('rmse'))} | {fmt(d.get('d1'))} | {fmt(d.get('d2'))} | "
                f"{fmt(d.get('d3'))} | {fmt(ev.get('scale_cv_raw'))} | "
                f"{fmt(ev.get('scale_cv_smoothed'))} |"
            )
        md.append(f"\nFrames evaluated: {s['R5']['depth_vs_sensor_all']['n_images']}")

    if "E" in a.steps:
        md += [
            "\n### Step E efficiency (CPU, this machine; fusion at 300x170 for Replica)",
            "",
            "| scene | run | wall s | peak RAM MB | mono ms/frame | align ms/frame | masks ms/frame | "
            "consistency ms/frame | fusion fps |",
            "|---|---|---|---|---|---|---|---|---|",
        ]
        for sc in a.scenes:
            s = run(sc, ["R5", "R6"])
            for k in ("R5", "R6"):
                t = s[k]["timings"]
                md.append(
                    f"| {sc} | {k} | {fmt(s[k]['wall_time_s'], 1)} | {fmt(s[k]['peak_ram_mb'], 0)} | "
                    f"{fmt(t.get('mono_inference_ms_per_frame'), 0)} | {fmt(t.get('align_ms_per_frame'), 1)} | "
                    f"{fmt(t.get('masks_ms_per_frame'), 1)} | {fmt(t.get('consistency_ms_per_frame'), 1)} | "
                    f"{fmt(t.get('fusion_fps'), 1)} |"
                )
        sc = a.scenes[0]
        md += [
            f"\n#### Voxel sweep ({sc}, R6 sensor pipeline)",
            "",
            "| voxel cm | F@2cm | F@5cm | acc cm | comp cm | fusion s | wall s | peak RAM MB | points |",
            "|---|---|---|---|---|---|---|---|---|",
        ]
        sweep = {}
        for v in SWEEP:
            ov = {"tsdf_p": {"voxel": v}}
            s = run(sc, ["R6"], ov, f"_v{v}")[f"R6_v{v}"]
            sweep[v] = s
            r = _r(s)
            md.append(
                f"| {v * 100:.0f} | {fmt(r.get('fscore_2cm'))} | {fmt(r.get('fscore_5cm'))} | "
                f"{fmt(r.get('accuracy'), 2, 100)} | {fmt(r.get('completion'), 2, 100)} | "
                f"{fmt(s['timings'].get('fusion_s'), 1)} | {fmt(s['wall_time_s'], 1)} | "
                f"{fmt(s['peak_ram_mb'], 0)} | {s['n_points']} |"
            )
        js["E_voxel_sweep"] = {str(k): v for k, v in sweep.items()}

    if "F" in a.steps:
        sc = a.scenes[0]
        md += [f"\n### Step F robustness ({sc})", "", HEAD_3D.replace("| run", "| variant")]
        base = {k: run(sc, [k])[k] for k in ("R5", "R6")}
        js["F"] = {}
        for k in ("R5", "R6"):
            md.append(row_3d(f"{k} baseline", base[k]))
            for nm, nz in NOISE.items():
                ev = run(sc, [k], {"pose_noise": nz}, f"_{nm}")[f"{k}_{nm}"]
                js["F"][f"{k}_{nm}"] = ev
                md.append(row_3d(f"{k} {nm}", ev))
            for nm, n in SUBSAMPLE.items():
                ev = run(sc, [k], {"max_frames": n}, f"_{nm}")[f"{k}_{nm}"]
                js["F"][f"{k}_{nm}"] = ev
                md.append(row_3d(f"{k} {nm} ({n} frames)", ev))
        md.append(
            "\nLow-texture / dark scene: NOT RUN (none of the downloaded scenes is dark or "
            "low-texture; recorded as a gap)."
        )

    if "G" in a.steps:
        sc = a.scenes[0]
        md += [
            f"\n### Step G repeatability ({sc}, R5)",
            "",
            "| run | F@2cm | F@5cm | acc cm | comp cm |",
            "|---|---|---|---|---|",
        ]
        same, seeds = [], []
        for i in range(3):
            ev = run(sc, ["R5"], {}, f"_rep{i}")[f"R5_rep{i}"]
            same.append(_r(ev).get("fscore_5cm"))
            md.append(
                f"| same seed #{i} | {fmt(_r(ev).get('fscore_2cm'))} | "
                f"{fmt(_r(ev).get('fscore_5cm'))} | {fmt(_r(ev).get('accuracy'), 2, 100)} | "
                f"{fmt(_r(ev).get('completion'), 2, 100)} |"
            )
        for i in range(3):
            ev = run(sc, ["R5"], {"align_p": {"seed": i + 1}}, f"_seed{i + 1}")[f"R5_seed{i + 1}"]
            seeds.append(_r(ev).get("fscore_5cm"))
            md.append(
                f"| RANSAC seed {i + 1} | {fmt(_r(ev).get('fscore_2cm'))} | "
                f"{fmt(_r(ev).get('fscore_5cm'))} | {fmt(_r(ev).get('accuracy'), 2, 100)} | "
                f"{fmt(_r(ev).get('completion'), 2, 100)} |"
            )
        md.append(
            f"\nF@5cm, identical seed x3: mean {np.mean(same):.4f} ± {np.std(same):.4f}; "
            f"different RANSAC seeds x3: mean {np.mean(seeds):.4f} ± {np.std(seeds):.4f}"
        )
        js["G"] = {"same_seed_f5": same, "diff_seed_f5": seeds}

    out = BENCH / "tables.md"
    out.write_text("\n".join(md) + "\n")
    (BENCH / "tables.json").write_text(json.dumps(js, indent=1, default=str))
    print("\n".join(md))
    print(f"\nwrote {out}")


if __name__ == "__main__":
    main()
