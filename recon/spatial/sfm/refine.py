"""Phase 2: pose refinement -> metric, gravity-aligned sparse model (docs/INTERFACES.md §2).

COLMAP (pycolmap) does the work: SIFT + sequential/spatial matching + incremental mapping
with position priors (prior-weighted BA). Our code: priors -> database, model selection,
robust Sim(3) to the metric priors, gravity alignment, bookkeeping.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import cv2
import numpy as np
import pycolmap

from formats.models import Priors
from spatial.geometry import invert, rotation_aligning
from spatial.stage import record_stage


def _T_world_cam(img: pycolmap.Image) -> np.ndarray:
    T = np.eye(4)
    T[:3, :] = img.cam_from_world().inverse().matrix()
    return T


def _transform_reconstruction(rec: pycolmap.Reconstruction, s: float, R: np.ndarray, t: np.ndarray):
    rec.transform(pycolmap.Sim3d(s, pycolmap.Rotation3d(R), t.reshape(3, 1)))


def depth_scale_factor(
    model: pycolmap.Reconstruction,
    ws: Path,
    pri: Priors,
    min_obs: int = 20,
    min_images: int = 5,
    depth_range_m: tuple[float, float] = (0.2, 6.0),
) -> dict:
    """Metric scale from sensor depth: per image, median of (sensor depth at a sparse keypoint)
    / (that point's depth in the current model); the global factor is the median over images.
    Independent of the (drifting) pose priors, so it fixes prior-inherited scale error."""
    per_image = []
    for img in model.images.values():
        if not img.has_pose:
            continue
        stem = Path(img.name).stem
        fr = next((f for f in pri.frames if f.id == stem), None)
        if fr is None or fr.depth is None:
            continue
        d_img = cv2.imread(str(ws / fr.depth), cv2.IMREAD_UNCHANGED)
        if d_img is None:
            continue
        d_img = d_img.astype(np.float64) * pri.depth_scale_m
        T_cw = np.linalg.inv(_T_world_cam(img))
        ratios = []
        for p2 in img.points2D:
            if not p2.has_point3D():
                continue
            u, v = int(np.floor(p2.xy[0])), int(np.floor(p2.xy[1]))
            if not (0 <= v < d_img.shape[0] and 0 <= u < d_img.shape[1]):
                continue
            d = d_img[v, u]
            z = (T_cw[:3, :3] @ model.points3D[p2.point3D_id].xyz + T_cw[:3, 3])[2]
            if depth_range_m[0] <= d <= depth_range_m[1] and z > 1e-6:
                ratios.append(d / z)
        if len(ratios) >= min_obs:
            per_image.append(float(np.median(ratios)))
    if len(per_image) < min_images:
        return {"status": "insufficient", "n_images": len(per_image)}
    s = float(np.median(per_image))
    mad = float(np.median(np.abs(np.asarray(per_image) - s)))
    return {"status": "ok", "scale": s, "n_images": len(per_image), "mad": mad}


def refine(
    ws: Path,
    run: Path,
    *,
    max_image_size: int = 1024,
    matcher: str = "auto",
    prior_sigma_m: float = 0.05,
    num_threads: int = -1,
    prior_mode: str = "sim3",
    seed: int = 0,
    sim3_max_error_m: float = 0.10,
    depth_scale: bool = False,
    mapper: str = "incremental",
) -> dict:
    """`mapper`: "incremental" (default) or "global" (COLMAP 4 integrated GLOMAP global mapper)."""
    if mapper not in ("incremental", "global"):
        raise ValueError(f"mapper must be incremental|global, got {mapper!r}")
    pri = Priors.model_validate_json((ws / "priors.json").read_text())
    # prior_mode: "sim3" = pure SfM, then robust Sim(3) to priors (priors don't bend the
    #             trajectory); "ba" = additionally use prior positions inside BA.
    params = dict(
        max_image_size=max_image_size,
        matcher=matcher,
        prior_sigma_m=prior_sigma_m,
        prior_mode=prior_mode,
        seed=seed,
        sim3_max_error_m=sim3_max_error_m,
        depth_scale=depth_scale,
        mapper=mapper,
    )
    with record_stage("phase2_refine", run / "phase2", params=params) as rec:
        work = run / "phase2" / "colmap"
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)
        db_path = work / "database.db"
        names = [Path(f.image).name for f in pri.frames]
        K = pri.intrinsics

        # 1. Features with the known pinhole intrinsics (fixed; ARKit/ARCore provide them).
        reader = pycolmap.ImageReaderOptions()
        reader.camera_model = "PINHOLE"
        reader.camera_params = f"{K.fx},{K.fy},{K.cx},{K.cy}"
        ext = pycolmap.FeatureExtractionOptions()
        ext.max_image_size = max_image_size
        ext.num_threads = num_threads
        pycolmap.set_random_seed(seed)
        pycolmap.extract_features(
            db_path,
            ws / "images",
            image_names=names,
            camera_mode=pycolmap.CameraMode.SINGLE,
            reader_options=reader,
            extraction_options=ext,
        )

        # 2. Position priors into the database.
        has_priors = pri.pose_source != "none"
        by_name = {Path(f.image).name: f for f in pri.frames}
        if has_priors:
            db = pycolmap.Database.open(db_path)
            for img in db.read_all_images():
                T = np.asarray(by_name[img.name].T_world_camera)
                db.write_pose_prior(
                    pycolmap.PosePrior(
                        corr_data_id=img.data_id,
                        position=T[:3, 3],
                        position_covariance=np.eye(3) * prior_sigma_m**2,
                        coordinate_system=pycolmap.PosePriorCoordinateSystem.CARTESIAN,
                    )
                )
            db.close()

        # 3. Matching: sequential always (video order); + spatial when priors exist.
        mode = matcher if matcher != "auto" else ("exhaustive" if len(names) <= 60 else "seq")
        if mode == "exhaustive":
            pycolmap.match_exhaustive(db_path)
        else:
            seq = pycolmap.SequentialPairingOptions()
            seq.overlap, seq.quadratic_overlap = 10, True
            pycolmap.match_sequential(db_path, pairing_options=seq)
            if has_priors:
                sp = pycolmap.SpatialPairingOptions()
                sp.max_num_neighbors, sp.ignore_z = 20, False
                pycolmap.match_spatial(db_path, pairing_options=sp)

        # 4. Incremental mapping; intrinsics fixed; prior-position BA when priors exist.
        opts = pycolmap.IncrementalPipelineOptions()
        opts.ba_refine_focal_length = False
        opts.ba_refine_principal_point = False
        opts.ba_refine_extra_params = False
        # Several models allowed (largest kept): with a single model, a poor initial pair
        # can end mapping after 2 images (observed on ETH3D office).
        opts.multiple_models = True
        opts.random_seed = seed
        pycolmap.set_random_seed(seed)
        if has_priors and prior_mode == "ba":
            opts.use_prior_position = True
            opts.use_robust_loss_on_prior_position = True
        raw_dir = ws / "sparse" / "raw"
        if raw_dir.exists():
            shutil.rmtree(raw_dir)
        raw_dir.mkdir(parents=True)
        if mapper == "global":
            gopts = pycolmap.GlobalPipelineOptions()
            models = pycolmap.global_mapping(db_path, ws / "images", raw_dir, gopts)
        else:
            models = pycolmap.incremental_mapping(db_path, ws / "images", raw_dir, options=opts)
        if not models:
            rec.status = "failed"
            rec.notes.append(f"{mapper} mapping produced no model")
            return {"status": "failed"}
        model = max(models.values(), key=lambda r: r.num_reg_images())
        rec.metrics["model_sizes"] = sorted(
            (r.num_reg_images() for r in models.values()), reverse=True
        )
        # incremental_mapping writes models into numbered subdirs; keep the chosen one as 'raw'.
        model.write(raw_dir)
        reg = {img.name: img for img in model.images.values() if img.has_pose}

        align = {
            "has_priors": has_priors,
            "pose_source": pri.pose_source,
            "registered": len(reg),
            "n_frames": len(names),
            "unregistered": sorted(Path(n).stem for n in names if n not in reg),
        }
        # 5. Robust Sim(3) raw -> prior world (metric) on camera centres.
        if has_priors:
            src = np.array([_T_world_cam(reg[n])[:3, 3] for n in reg])
            dst = np.array([np.asarray(by_name[n].T_world_camera)[:3, 3] for n in reg])
            ransac = pycolmap.RANSACOptions()
            ransac.max_error = sim3_max_error_m
            res = pycolmap.estimate_sim3d_robust(src, dst, ransac)
            if not res:
                rec.status = "failed"
                rec.notes.append("Sim(3) to priors failed")
                return {"status": "failed"}
            M = np.asarray(res["tgt_from_src"].matrix())
            s = float(np.cbrt(np.linalg.det(M[:3, :3])))
            R, t = M[:3, :3] / s, M[:3, 3]
            resid = np.linalg.norm((s * src @ R.T + t) - dst, axis=1)
            _transform_reconstruction(model, s, R, t)
            align |= {
                "sim3_scale": s,
                "sim3_R": R.tolist(),
                "sim3_t": t.tolist(),
                "prior_residual_median_m": float(np.median(resid)),
                "prior_residual_p95_m": float(np.percentile(resid, 95)),
                "prior_inliers": int((resid < sim3_max_error_m).sum()),
            }
            if depth_scale:
                ds = depth_scale_factor(model, ws, pri)
                align["depth_scale"] = ds
                if ds["status"] == "ok":
                    centres = np.array(
                        [_T_world_cam(i)[:3, 3] for i in model.images.values() if i.has_pose]
                    )
                    c = centres.mean(0)  # scale about the trajectory centroid
                    _transform_reconstruction(model, ds["scale"], np.eye(3), (1 - ds["scale"]) * c)
                    align["scale_source"] = "sensor_depth"
                else:
                    rec.notes.append(f"depth scale skipped: {ds}")
            # 6. Gravity: rotate prior world so gravity -> -Z (our world frame).
            if pri.gravity_world is not None:
                Rg = rotation_aligning(np.asarray(pri.gravity_world), np.array([0, 0, -1.0]))
                _transform_reconstruction(model, 1.0, Rg, np.zeros(3))
                align |= {"gravity_aligned": True, "R_world_prior": Rg.tolist()}
            else:
                align |= {"gravity_aligned": False, "R_world_prior": np.eye(3).tolist()}
            align["metric"] = True
        else:
            align |= {
                "metric": False,
                "gravity_aligned": False,
                "note": "no priors: arbitrary SfM scale/frame (not metric)",
            }

        out = ws / "sparse" / "0"
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        model.write(out)
        poses = {
            Path(img.name).stem: _T_world_cam(img).tolist()
            for img in model.images.values()
            if img.has_pose
        }
        (ws / "poses_refined.json").write_text(json.dumps(poses))
        (ws / "alignment.json").write_text(json.dumps(align, indent=1))
        rec.metrics |= {
            "registered": len(reg),
            "n_frames": len(names),
            "reg_ratio": len(reg) / len(names),
            "n_points3D": model.num_points3D(),
            "mean_reproj_px": float(model.compute_mean_reprojection_error()),
            "mean_track_length": float(model.compute_mean_track_length()),
        }
        rec.metrics |= {k: v for k, v in align.items() if k.startswith(("prior_", "sim3_scale"))}
        for p in [ws / "poses_refined.json", ws / "alignment.json"]:
            rec.add_output(p)
        return align | rec.metrics


def poses_in_gt_frame(ws: Path) -> dict[str, np.ndarray]:
    """Refined poses expressed back in the prior/GT world (undo gravity rotation) for eval."""
    align = json.loads((ws / "alignment.json").read_text())
    Rg = np.asarray(align.get("R_world_prior", np.eye(3)))
    G = np.eye(4)
    G[:3, :3] = Rg.T
    poses = json.loads((ws / "poses_refined.json").read_text())
    return {k: G @ np.asarray(v) for k, v in poses.items()}


__all__ = ["refine", "poses_in_gt_frame", "invert"]
