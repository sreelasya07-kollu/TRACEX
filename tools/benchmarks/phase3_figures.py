"""Phase 3 evidence images -> docs/TEST_EVIDENCE/phase3/ (Step G).

  python -m benchmarks.phase3_figures --scene room0
1. error_heatmap_<run>_view{0,1,2}.png  fused points coloured by distance to culled GT (cm)
2. depth_<run>_<n>.png                  RGB | GT depth | predicted depth | abs error (5 frames)
3. voxel_tradeoff.png                   F@5cm / time / RAM / points vs voxel size
All inputs are existing runs under runs/bench_phase3/<scene>/ (nothing is recomputed except GT
sampling for the heatmap).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import open3d as o3d  # noqa: E402
from scipy.spatial import cKDTree  # noqa: E402

from benchmarks.phase3 import BENCH  # noqa: E402
from eval.gt import load_mesh, resolve_gt_path  # noqa: E402
from eval.recon_metrics import cull_to_cameras, sample_mesh  # noqa: E402

OUT = Path(__file__).resolve().parents[1] / "docs" / "TEST_EVIDENCE" / "phase3"


def heatmaps(scene: str, run: str, max_cm: float = 10.0, n_show: int = 150_000):
    ws = BENCH / scene / "_workspace" / "workspace"
    out = BENCH / scene / run / "phase3"
    ev = json.loads((BENCH / scene / run / "eval.json").read_text())
    meta = json.loads((out / "meta.json").read_text())
    ids = meta["metrics"]["frame_ids"]
    ref = json.loads((ws / "gt" / "gt_ref.json").read_text())
    pri = json.loads((ws / "priors.json").read_text())
    D = meta["params"]["downscale"]
    intr = pri["intrinsics"]
    K = np.array(
        [
            [intr["fx"] / D, 0, (intr["cx"] + 0.5) / D - 0.5],
            [0, intr["fy"] / D, (intr["cy"] + 0.5) / D - 0.5],
            [0, 0, 1],
        ]
    )
    W, H = meta["metrics"]["width"], meta["metrics"]["height"]
    gt_poses = json.loads((ws / "gt" / "poses_gt.json").read_text())
    mesh = load_mesh(resolve_gt_path(ref["geometry"]))
    pc = sample_mesh(mesh, 400_000, 0)
    keep = cull_to_cameras(
        np.asarray(pc.points), [(np.asarray(gt_poses[i]), K, W, H) for i in ids], gt_mesh=mesh
    )
    gt = np.asarray(pc.points)[keep]

    pcd = o3d.io.read_point_cloud(str(out / "fusion" / "cloud.ply"))
    al = np.asarray(json.loads((ws / "alignment.json").read_text()).get("R_world_prior", np.eye(3)))
    pts = np.asarray(pcd.points) @ al  # our world -> prior world (R^T applied to row vectors)
    R, t = np.asarray(ev["traj_align"]["R"]), np.asarray(ev["traj_align"]["t"])
    pts = pts @ R.T + t
    cams = [(np.asarray(gt_poses[i]), K, W, H) for i in ids]
    pts = pts[cull_to_cameras(pts, cams, gt_mesh=None)]
    d_cm = cKDTree(gt).query(pts, workers=-1)[0] * 100
    rng = np.random.default_rng(0)
    sel = rng.choice(len(pts), min(n_show, len(pts)), replace=False)
    p, d = pts[sel], np.clip(d_cm[sel], 0, max_cm)
    lo, hi = np.percentile(p, 1, axis=0), np.percentile(p, 99, axis=0)
    views = [("top (x-y)", (0, 1)), ("front (x-z)", (0, 2)), ("side (y-z)", (1, 2))]
    OUT.mkdir(parents=True, exist_ok=True)
    for n, (name, (a, b)) in enumerate(views):
        fig, ax = plt.subplots(figsize=(7, 5), dpi=130)
        order = np.argsort(d)  # draw large errors last
        sc = ax.scatter(
            p[order, a],
            p[order, b],
            c=d[order],
            s=0.6,
            cmap="turbo",
            vmin=0,
            vmax=max_cm,
            linewidths=0,
        )
        ax.set_xlim(lo[a], hi[a])
        ax.set_ylim(lo[b], hi[b])
        ax.set_aspect("equal")
        ax.set_title(
            f"{scene}/{run}: distance to GT, {name}\n"
            f"median {np.median(d_cm):.2f} cm, p95 {np.percentile(d_cm, 95):.2f} cm "
            f"({len(pts)} pts)"
        )
        fig.colorbar(sc, label=f"cm (clipped at {max_cm:.0f})")
        fig.savefig(OUT / f"error_heatmap_{scene}_{run}_view{n}.png", bbox_inches="tight")
        plt.close(fig)


def depth_panels(scene: str, run: str, n: int = 5):
    ws = BENCH / scene / "_workspace" / "workspace"
    out = BENCH / scene / run / "phase3"
    ids = json.loads((out / "meta.json").read_text())["metrics"]["frame_ids"]
    pri = {f["id"]: f for f in json.loads((ws / "priors.json").read_text())["frames"]}
    pick = [ids[i] for i in np.linspace(0, len(ids) - 1, n).round().astype(int)]
    OUT.mkdir(parents=True, exist_ok=True)
    for k, i in enumerate(pick):
        pred = cv2.imread(str(out / "depth_aligned" / f"{i}.png"), cv2.IMREAD_UNCHANGED) / 1000.0
        valid = cv2.imread(str(out / "depth_valid" / f"{i}.png"), cv2.IMREAD_UNCHANGED) > 0
        H, W = pred.shape
        rgb = cv2.cvtColor(
            cv2.resize(cv2.imread(str(ws / pri[i]["image"])), (W, H), interpolation=cv2.INTER_AREA),
            cv2.COLOR_BGR2RGB,
        )
        gt = (
            cv2.resize(
                cv2.imread(str(ws / pri[i]["depth"]), cv2.IMREAD_UNCHANGED),
                (W, H),
                interpolation=cv2.INTER_NEAREST,
            )
            / 1000.0
        )
        err = np.where((gt > 0) & (pred > 0), np.abs(pred - gt), np.nan)
        vmax = max(np.nanpercentile(gt[gt > 0], 98), 1.0)
        fig, ax = plt.subplots(1, 4, figsize=(16, 3.2), dpi=120)
        ax[0].imshow(rgb)
        ax[0].set_title(f"RGB {i}")
        ax[1].imshow(np.where(gt > 0, gt, np.nan), cmap="viridis", vmin=0, vmax=vmax)
        ax[1].set_title("GT / sensor depth (m)")
        im2 = ax[2].imshow(np.where(pred > 0, pred, np.nan), cmap="viridis", vmin=0, vmax=vmax)
        ax[2].set_title(f"predicted+aligned (valid {valid.mean() * 100:.0f}%)")
        im3 = ax[3].imshow(err, cmap="magma", vmin=0, vmax=0.3)
        ax[3].set_title(f"|error| (m), median {np.nanmedian(err):.3f}")
        for a in ax:
            a.axis("off")
        fig.colorbar(im2, ax=ax[2], fraction=0.046)
        fig.colorbar(im3, ax=ax[3], fraction=0.046)
        fig.savefig(OUT / f"depth_{scene}_{run}_{k}.png", bbox_inches="tight")
        plt.close(fig)


def voxel_plot(scene: str):
    rows = []
    for v in (0.01, 0.02, 0.04):
        p = BENCH / scene / f"R6_v{v}" / "eval.json"
        if p.exists():
            rows.append((v, json.loads(p.read_text())))
    if not rows:
        return
    x = [r[0] * 100 for r in rows]
    fig, ax = plt.subplots(1, 4, figsize=(15, 3.2), dpi=120)
    series = [
        ("F-score @5 cm", [r[1]["recon"]["fscore_5cm"] for r in rows]),
        ("fusion time (s)", [r[1]["timings"]["fusion_s"] for r in rows]),
        ("peak RAM (MB)", [r[1]["peak_ram_mb"] for r in rows]),
        ("points", [r[1]["n_points"] for r in rows]),
    ]
    for a, (title, y) in zip(ax, series, strict=True):
        a.plot(x, y, "o-")
        a.set_xscale("log")
        a.set_xticks(x, [f"{v:g}" for v in x])
        a.set_xlabel("voxel (cm)")
        a.set_title(title)
        a.grid(alpha=0.3)
    OUT.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT / "voxel_tradeoff.png", bbox_inches="tight")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scene", default="room0")
    ap.add_argument("--run", default="R5")
    a = ap.parse_args()
    depth_panels(a.scene, a.run)
    voxel_plot(a.scene)
    heatmaps(a.scene, a.run)
    heatmaps(a.scene, "R6")
    print("figures ->", OUT, sorted(p.name for p in OUT.glob("*.png")))


if __name__ == "__main__":
    main()
