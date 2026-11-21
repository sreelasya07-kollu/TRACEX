"""Phase 3 orchestration: depth source -> alignment -> masks -> consistency -> TSDF -> clean.

Output layout: docs/INTERFACES.md §3. All poses/clouds are in OUR world frame (the Phase 2
gravity-aligned frame). `pose_source="gt"` (oracle only) maps GT poses into that frame
with the same gravity rotation Phase 2 used.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

import cv2
import numpy as np
import open3d as o3d

try:
    import pycolmap
except (ImportError, RuntimeError):
    pycolmap = None

from formats.models import DepthAlignment, DepthModelInfo, FrameAlignment, Priors
from spatial.depth.align import (
    AlignParams,
    apply_alignment,
    fit_scale,
    sample_bilinear,
    smooth_scales,
)
from spatial.depth.consistency import ConsistencyParams, consistency_mask, select_neighbours
from spatial.depth.masks import MaskParams, build_mask
from spatial.depth.mvs import MVSParams, load_mvs_depths, run_openmvs_depth
from spatial.depth.sources import MonoDepth, SensorDepth
from spatial.fusion.clean import CleanParams, clean
from spatial.fusion.tsdf import TSDF, TSDFParams
from spatial.stage import record_stage, sha256_file


@dataclass
class Phase3Config:
    depth_source: str = "auto"  # sensor | mvs | mono | auto (sensor if present, else mvs)
    model: str = "small"
    model_kind: str = "metric"  # metric | relative
    input_size: int = 518
    device: str = "cpu"
    downscale: int = 1  # relative to workspace images
    max_frames: int | None = None
    align: bool = True
    global_scale: bool = False
    masks: bool = True
    consistency: bool = True
    pose_source: str = "refined"  # refined | gt (oracle only)
    mvs_conf_min: float = 0.5  # OpenMVS confidence cutoff (TUM: 0.5 -> AbsRel 0.070, 63 % kept)
    seed: int = 0
    # Step F robustness: (translation sigma [m], rotation sigma [deg]) white noise per frame
    # added to the poses used for fusion/consistency/alignment. None = off.
    pose_noise: tuple[float, float] | None = None
    align_p: AlignParams = field(default_factory=AlignParams)
    mvs_p: MVSParams = field(default_factory=MVSParams)
    mask_p: MaskParams = field(default_factory=MaskParams)
    cons_p: ConsistencyParams = field(default_factory=ConsistencyParams)
    tsdf_p: TSDFParams = field(default_factory=TSDFParams)
    clean_p: CleanParams = field(default_factory=CleanParams)


def perturb_poses(poses: dict[str, np.ndarray], trans_m: float, rot_deg: float, seed: int):
    """Per-frame white pose noise: world-frame translation N(0, trans_m) and a camera-frame
    rotation vector N(0, rot_deg) per axis. Deterministic given `seed` and sorted frame ids."""
    from scipy.spatial.transform import Rotation

    rng = np.random.default_rng(seed)
    out = {}
    for k in sorted(poses):
        T = poses[k].copy()
        T[:3, 3] += rng.normal(0, trans_m, 3)
        T[:3, :3] = (
            T[:3, :3] @ Rotation.from_rotvec(rng.normal(0, np.radians(rot_deg), 3)).as_matrix()
        )
        out[k] = T
    return out


def _scaled_K(K: np.ndarray, D: int) -> np.ndarray:
    K2 = K.copy()
    K2[0, 0] /= D
    K2[1, 1] /= D
    K2[0, 2] = (K[0, 2] + 0.5) / D - 0.5
    K2[1, 2] = (K[1, 2] + 0.5) / D - 0.5
    return K2


def load_poses(ws: Path, source: str) -> dict[str, np.ndarray]:
    if source == "refined":
        return {
            k: np.asarray(v) for k, v in json.loads((ws / "poses_refined.json").read_text()).items()
        }
    # Oracle: GT poses, mapped into our world frame (same gravity rotation as Phase 2).
    align = (
        json.loads((ws / "alignment.json").read_text()) if (ws / "alignment.json").exists() else {}
    )
    G = np.eye(4)
    G[:3, :3] = np.asarray(align.get("R_world_prior", np.eye(3)))
    gt = json.loads((ws / "gt" / "poses_gt.json").read_text())
    return {k: G @ np.asarray(v) for k, v in gt.items()}


def _sparse_observations(ws: Path) -> dict[str, tuple[np.ndarray, np.ndarray]]:
    """frame id -> (uv keypoints [N,2] at workspace resolution, 3D points world [N,3])."""
    rec = pycolmap.Reconstruction(ws / "sparse" / "0")
    pts = {pid: p.xyz for pid, p in rec.points3D.items()}
    out = {}
    for img in rec.images.values():
        if not img.has_pose:
            continue
        uv, X = [], []
        for p2 in img.points2D:
            if p2.has_point3D():
                uv.append(p2.xy)
                X.append(pts[p2.point3D_id])
        out[Path(img.name).stem] = (
            np.asarray(uv, float).reshape(-1, 2),
            np.asarray(X, float).reshape(-1, 3),
        )
    return out


def select_frames(ids: list[str], max_frames: int | None) -> list[str]:
    ids = sorted(ids)
    if max_frames is None or max_frames >= len(ids):
        return ids
    return [ids[i] for i in np.linspace(0, len(ids) - 1, max_frames).round().astype(int)]


def run_phase3(ws: Path, out: Path, cfg: Phase3Config) -> dict:
    params = {
        k: (asdict(v) if hasattr(v, "__dataclass_fields__") else v) for k, v in vars(cfg).items()
    }
    with record_stage("phase3", out, device=cfg.device, params=params) as rec:
        pri = Priors.model_validate_json((ws / "priors.json").read_text())
        poses = load_poses(ws, cfg.pose_source)
        if cfg.pose_noise is not None:
            poses = perturb_poses(poses, *cfg.pose_noise, cfg.seed)
        by_id = {f.id: f for f in pri.frames}
        ids = select_frames([i for i in poses if i in by_id], cfg.max_frames)
        rec.metrics["frame_ids"] = ids
        D = cfg.downscale
        W, H = pri.width // D, pri.height // D
        K0 = pri.intrinsics.K()
        K = _scaled_K(K0, D)
        has_sensor = all(by_id[i].depth for i in ids)
        src = (
            cfg.depth_source if cfg.depth_source != "auto" else ("sensor" if has_sensor else "mvs")
        )
        if src == "sensor" and not has_sensor:
            raise ValueError("depth_source=sensor but workspace has no sensor depth")
        if src == "mono" and not cfg.align and cfg.model_kind != "metric":
            raise ValueError("relative depth without alignment is meaningless")
        for d in ("depth_aligned", "depth_valid", "fusion"):
            (out / d).mkdir(parents=True, exist_ok=True)

        rgb = {
            i: cv2.cvtColor(
                cv2.resize(
                    cv2.imread(str(ws / by_id[i].image)), (W, H), interpolation=cv2.INTER_AREA
                ),
                cv2.COLOR_BGR2RGB,
            )
            for i in ids
        }
        t = {}
        # ---- 1. depth source ------------------------------------------------------------
        t0 = time.perf_counter()
        frames_al: dict[str, FrameAlignment] = {}
        if src == "sensor":
            sd = SensorDepth(cfg.mask_p.min_m, cfg.mask_p.max_m)
            depth = {
                i: cv2.resize(sd.load(ws / by_id[i].depth), (W, H), interpolation=cv2.INTER_NEAREST)
                for i in ids
            }
            model_info = DepthModelInfo(
                name="sensor", hf_id=None, revision=None, kind="sensor", licence="dataset"
            )
            for i in ids:
                frames_al[i] = FrameAlignment(
                    source="sensor", scale=1.0, shift=0.0, status="no_alignment"
                )
            t["depth_source_ms_per_frame"] = (time.perf_counter() - t0) * 1e3 / len(ids)
        elif src in ("mvs", "hybrid"):
            mvs_dir = out / "mvs"
            if not list(mvs_dir.glob("depth*.dmap")):
                run_openmvs_depth(ws, mvs_dir, cfg.mvs_p, D)
            raw = load_mvs_depths(mvs_dir, (W, H))
            depth = {}
            for i in ids:
                d, c = raw.get(i, (np.zeros((H, W), np.float32), None))
                if c is not None:
                    d = np.where(c >= cfg.mvs_conf_min, d, 0).astype(np.float32)
                depth[i] = d
                frames_al[i] = FrameAlignment(
                    source="mvs",
                    scale=1.0,
                    shift=0.0,
                    status="no_alignment" if i in raw else "rejected_few_points",
                )
            model_info = DepthModelInfo(
                name="openmvs", hf_id=None, revision=None, kind="mvs", licence="AGPL-3.0 (tool)"
            )
            t["mvs_s"] = time.perf_counter() - t0
            if src == "hybrid":  # fill MVS holes with mono depth scale-aligned to MVS
                mono = MonoDepth(cfg.model, "metric", cfg.device, cfg.input_size)
                preds = dict(
                    zip(ids, mono.predict([ws / by_id[i].image for i in ids]), strict=True)
                )
                rng = np.random.default_rng(cfg.seed)
                filled = 0
                for i in ids:
                    d = depth[i]
                    pm = cv2.resize(preds[i], (W, H), interpolation=cv2.INTER_LINEAR)
                    ys, xs = np.nonzero(d > 0)
                    if len(ys) < 200:
                        continue
                    sel = rng.choice(len(ys), min(3000, len(ys)), replace=False)
                    fit = fit_scale(
                        d[ys[sel], xs[sel]], pm[ys[sel], xs[sel]], "metric", cfg.align_p
                    )
                    if fit["status"] != "ok":
                        continue
                    hole = d == 0
                    d = d.copy()
                    d[hole] = (pm * fit["scale"])[hole]
                    filled += int(hole.sum())
                    depth[i] = d.astype(np.float32)
                rec.metrics["hybrid_filled_px"] = filled
        else:
            mono = MonoDepth(cfg.model, cfg.model_kind, cfg.device, cfg.input_size)
            preds = dict(zip(ids, mono.predict([ws / by_id[i].image for i in ids]), strict=True))
            info = mono.info()
            model_info = DepthModelInfo(
                name=info["name"],
                hf_id=info["hf_id"],
                revision=info["revision"],
                kind=info["kind"],
                licence=info["licence"],
            )
            t["mono_inference_ms_per_frame"] = (
                float(np.mean(mono.timings_ms)) if mono.timings_ms else None
            )
            t["mono_cache_hits"] = len(ids) - (
                len(mono.timings_ms) * mono.batch if mono.timings_ms else 0
            )
            # ---- 2. alignment -----------------------------------------------------------
            t1 = time.perf_counter()
            kind = info["kind"]
            fits = {}
            if cfg.align:
                obs = _sparse_observations(ws)
                for i in ids:
                    uv, X = obs.get(i, (np.zeros((0, 2)), np.zeros((0, 3))))
                    T_cw = (
                        np.linalg.inv(poses[i])
                        if cfg.pose_source == "refined"
                        else np.linalg.inv(load_poses(ws, "refined")[i])
                    )
                    z = (X @ T_cw[:3, :3].T + T_cw[:3, 3])[:, 2] if len(X) else np.zeros(0)
                    p = sample_bilinear(preds[i], uv) if len(uv) else np.zeros(0)
                    fits[i] = fit_scale(z, p, kind, cfg.align_p)
                ok_ids = [i for i in ids if fits[i]["status"] == "ok"]
                scales = [fits[i]["scale"] if i in ok_ids else None for i in ids]
                sm = smooth_scales(scales, cfg.align_p.smooth_window)
                g = float(np.median([fits[i]["scale"] for i in ok_ids])) if ok_ids else None
                for i, s_sm in zip(ids, sm, strict=True):
                    f = fits[i]
                    frames_al[i] = FrameAlignment(
                        source="mono",
                        scale=f["scale"],
                        shift=f["shift"],
                        scale_smoothed=s_sm,
                        inliers=f["inliers"],
                        n_points=f["n_points"],
                        residual_m=f["residual_m"],
                        status=f["status"],
                    )
                rec.metrics["global_scale"] = g
            else:
                for i in ids:
                    frames_al[i] = FrameAlignment(
                        source="mono", scale=1.0, shift=0.0, status="no_alignment"
                    )
            depth = {}
            for i in ids:
                fa = frames_al[i]
                if cfg.align and fa.status != "ok":
                    depth[i] = np.zeros((H, W), np.float32)  # rejected frame: not fused
                    continue
                s = (
                    (rec.metrics["global_scale"] if cfg.global_scale else fa.scale_smoothed)
                    if cfg.align
                    else 1.0
                )
                d = apply_alignment(preds[i], kind, s, fa.shift or 0.0)
                depth[i] = cv2.resize(d, (W, H), interpolation=cv2.INTER_NEAREST)
            t["align_ms_per_frame"] = (time.perf_counter() - t1) * 1e3 / len(ids)

        # ---- 3. masks + 4. consistency ---------------------------------------------------
        t2 = time.perf_counter()
        valid, mstats = {}, {}
        for i in ids:
            valid[i], mstats[i] = build_mask(depth[i], K, cfg.mask_p, enabled=cfg.masks)
        t["masks_ms_per_frame"] = (time.perf_counter() - t2) * 1e3 / len(ids)
        t3 = time.perf_counter()
        cstats = {}
        if cfg.consistency:
            T_list = [poses[i] for i in ids]
            neigh = select_neighbours(T_list, cfg.cons_p)
            dl = [depth[i] for i in ids]
            vl = [valid[i] for i in ids]
            newv = {}
            for k, i in enumerate(ids):
                newv[i], cstats[i] = consistency_mask(k, dl, vl, T_list, K, neigh[k], cfg.cons_p)
            valid = newv
        t["consistency_ms_per_frame"] = (time.perf_counter() - t3) * 1e3 / len(ids)

        for i in ids:
            d = np.where(valid[i], depth[i], 0)
            cv2.imwrite(
                str(out / "depth_aligned" / f"{i}.png"),
                np.clip(np.round(depth[i] * 1000), 0, 65535).astype(np.uint16),
            )
            cv2.imwrite(str(out / "depth_valid" / f"{i}.png"), (valid[i] * 255).astype(np.uint8))
            depth[i] = d
        DA = DepthAlignment(
            model=model_info,
            global_scale=rec.metrics.get("global_scale") if cfg.global_scale else None,
            frames=frames_al,
        )
        (out / "depth_alignment.json").write_text(DA.model_dump_json(indent=1))

        # ---- 5. fusion + 6. cleanup ------------------------------------------------------
        t4 = time.perf_counter()
        vol = TSDF(cfg.tsdf_p)
        for i in ids:
            vol.integrate(depth[i], rgb[i], K, poses[i])
        pcd, mesh = vol.extract()
        tf = time.perf_counter() - t4
        t |= {"fusion_s": tf, "fusion_fps": len(ids) / tf}
        pcd, clstats = clean(pcd, cfg.tsdf_p.voxel, cfg.clean_p)
        o3d.io.write_point_cloud(str(out / "fusion" / "cloud.ply"), pcd)
        o3d.io.write_triangle_mesh(str(out / "fusion" / "tsdf_mesh.ply"), mesh)

        valid_frac = float(np.mean([valid[i].mean() for i in ids]))
        statuses = [f.status for f in frames_al.values()]
        rec.metrics |= {
            "depth_source": src,
            "n_frames": len(ids),
            "width": W,
            "height": H,
            "timings": t,
            "valid_fraction": valid_frac,
            "frames_ok": statuses.count("ok"),
            "frames_rejected": sum(s.startswith("rejected") for s in statuses),
            "mask_removed_mean": {
                k: float(np.mean([m[k] for m in mstats.values()]))
                for k in next(iter(mstats.values()))
            },
            "consistency_kept_fraction": (
                float(
                    np.sum([c["kept"] for c in cstats.values()])
                    / max(1, np.sum([c["input"] for c in cstats.values()]))
                )
                if cstats
                else None
            ),
            "clean": clstats,
            "n_points": len(pcd.points),
            "n_mesh_vertices": len(mesh.vertices),
        }
        if cfg.align and src == "mono":
            s = np.array([f.scale for f in frames_al.values() if f.status == "ok"])
            ss = np.array([f.scale_smoothed for f in frames_al.values() if f.status == "ok"])
            if len(s) > 1:
                rec.metrics["scale_cv_raw"] = float(s.std() / s.mean())
                rec.metrics["scale_cv_smoothed"] = float(ss.std() / ss.mean())
        for p in [
            out / "fusion" / "cloud.ply",
            out / "fusion" / "tsdf_mesh.ply",
            out / "depth_alignment.json",
        ]:
            rec.add_output(p)
        (out / "poses_used.json").write_text(json.dumps({i: poses[i].tolist() for i in ids}))
        rec.add_input(ws / "priors.json")
        if (ws / "poses_refined.json").exists():
            rec.add_input(ws / "poses_refined.json")
        return rec.metrics


__all__ = ["Phase3Config", "run_phase3", "load_poses", "sha256_file"]
