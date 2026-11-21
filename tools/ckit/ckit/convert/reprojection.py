"""Reprojection report: triangulate with the bundle's own poses (pycolmap) and measure error.

Per bundle: export-colmap -> SIFT -> sequential matching -> point triangulation with fixed poses ->
per-observation reprojection residuals. A principal-point sweep (cx, cy shifted by -0.5/0/+0.5 px)
tests ADR 0003: the 0.0 shift must give the lowest error; a consistent +-0.5 winner means the
platform intrinsics use the other pixel-centre convention.
"""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

import numpy as np

from ckit.convert.colmap import export_colmap

SHIFTS = (-0.5, 0.0, 0.5)


def _residuals(rec) -> np.ndarray:
    res = []
    for pid in rec.point3D_ids():
        pt = rec.point3D(pid)
        for el in pt.track.elements:
            img = rec.image(el.image_id)
            proj = img.project_point(pt.xyz)
            if proj is None:
                continue
            res.append(np.asarray(img.point2D(el.point2D_idx).xy) - np.asarray(proj))
    return np.asarray(res).reshape(-1, 2)


def _triangulate(
    pycolmap,
    model: Path,
    db: Path,
    images: Path,
    out: Path,
    shift: float,
    threads: int | None = None,
):
    rec = pycolmap.Reconstruction(str(model))
    for cam in rec.cameras.values():
        p = np.array(cam.params, float)
        p[2] += shift
        p[3] += shift
        cam.params = p
    out.mkdir(parents=True, exist_ok=True)
    opts = pycolmap.IncrementalPipelineOptions()
    opts.triangulation.random_seed = 0
    if threads is not None:
        opts.num_threads = threads
    return pycolmap.triangulate_points(rec, str(db), str(images), str(out), options=opts)


def _refine_principal_point(pycolmap, rec) -> list[float]:
    """BA with poses and focal fixed, principal point + points free; returns (dcx, dcy)."""
    cam = next(iter(rec.cameras.values()))
    before = np.array(cam.params, float)
    o = pycolmap.BundleAdjustmentOptions()
    o.refine_rig_from_world = False
    o.refine_sensor_from_rig = False
    o.refine_focal_length = False
    o.refine_extra_params = False
    o.refine_principal_point = True
    o.refine_points3D = True
    o.print_summary = False
    pycolmap.bundle_adjustment(rec, o)
    after = np.array(next(iter(rec.cameras.values())).params, float)
    return [round(float(after[i] - before[i]), 4) for i in (2, 3)]


def report_bundle(
    bundle: Path, work: Path | None = None, overlap: int = 10, num_threads: int | None = None
) -> dict:
    """num_threads=None keeps the fast defaults; an int pins extraction, matching and
    triangulation to that many threads (recon-bench uses 1 for run-to-run determinism)."""
    import pycolmap

    pycolmap.logging.minloglevel = 2  # warnings+ only
    pycolmap.logging.logtostderr = True  # no glog files in /tmp

    tmp = Path(tempfile.mkdtemp(prefix="ckit_reproj_")) if work is None else work
    try:
        model = tmp / "model"
        exp = export_colmap(bundle, model)
        db = tmp / "database.db"
        if db.exists():
            db.unlink()
        rec0 = pycolmap.Reconstruction(str(model))
        if rec0.num_cameras() != 1:
            raise ValueError(f"{bundle.name}: {rec0.num_cameras()} cameras; report needs exactly 1")
        cam0 = next(iter(rec0.cameras.values()))
        names = [img.name for img in rec0.images.values()]
        ext = pycolmap.FeatureExtractionOptions()
        ext.num_threads = 4 if num_threads is None else num_threads
        reader = pycolmap.ImageReaderOptions()
        reader.camera_model = cam0.model.name
        reader.camera_params = ",".join(repr(float(v)) for v in cam0.params)
        pycolmap.extract_features(
            db,
            bundle,
            image_names=names,
            camera_mode=pycolmap.CameraMode.SINGLE,
            reader_options=reader,
            extraction_options=ext,
            device=pycolmap.Device.cpu,
        )
        # Extraction is parallel, so DB image IDs are unordered: re-export with the DB's IDs.
        dbh = pycolmap.Database.open(str(db))
        ids = {img.name: img.image_id for img in dbh.read_all_images()}
        dbh.close()
        export_colmap(bundle, model, image_ids=ids)
        pair = pycolmap.SequentialPairingOptions()
        pair.overlap = overlap
        match_kw = {}
        if num_threads is not None:  # default call path stays exactly as before
            match = pycolmap.FeatureMatchingOptions()
            match.num_threads = num_threads
            match_kw["matching_options"] = match
        pycolmap.match_sequential(db, pairing_options=pair, device=pycolmap.Device.cpu, **match_kw)
        sweep = {}
        main = None
        for s in SHIFTS:
            rec = _triangulate(pycolmap, model, db, bundle, tmp / f"tri_{s:+.1f}", s, num_threads)
            r = _residuals(rec)
            e = np.linalg.norm(r, axis=1) if len(r) else np.zeros(0)
            sweep[f"{s:+.1f}"] = round(float(e.mean()), 4) if len(e) else None
            if s == 0.0:
                main = (rec, r, e)
        rec, r, e = main
        # Poses fixed, principal point free: its shift directly estimates a pixel-centre bias.
        ref = _triangulate(pycolmap, model, db, bundle, tmp / "tri_pp", 0.0, num_threads)
        pp_delta = _refine_principal_point(pycolmap, ref)
        observed = {
            el.image_id for pid in rec.point3D_ids() for el in rec.point3D(pid).track.elements
        }
        finite = {k: v for k, v in sweep.items() if v is not None}
        return {
            "bundle": bundle.name,
            "images": exp["images"],
            "skipped_no_pose": exp["skipped"],
            "images_with_points": len(observed),
            "points3D": rec.num_points3D(),
            "observations": int(len(e)),
            "reproj_mean_px": round(float(e.mean()), 4) if len(e) else None,
            "reproj_median_px": round(float(np.median(e)), 4) if len(e) else None,
            "reproj_p90_px": round(float(np.percentile(e, 90)), 4) if len(e) else None,
            "mean_signed_residual_px": [round(float(v), 4) for v in r.mean(axis=0)]
            if len(r)
            else None,
            "principal_point_sweep_mean_px": sweep,
            "best_shift_px": min(finite, key=finite.get) if finite else None,
            "refined_principal_point_delta_px": pp_delta,
        }
    finally:
        if work is None:
            shutil.rmtree(tmp, ignore_errors=True)


def report(bundles: list[Path]) -> dict:
    """ADR 0003 holds when |refined_principal_point_delta_px| << 0.5 on every bundle."""
    rows = [report_bundle(b) for b in bundles]
    means = [r["reproj_mean_px"] for r in rows if r["reproj_mean_px"] is not None]
    return {
        "bundles": rows,
        "summary": {
            "n": len(rows),
            "mean_of_means_px": round(float(np.mean(means)), 4) if means else None,
            "max_mean_px": round(float(np.max(means)), 4) if means else None,
            "max_abs_pp_delta_px": max(
                (abs(v) for r in rows for v in r["refined_principal_point_delta_px"]), default=None
            ),
            "best_shift_votes": {
                s: sum(1 for r in rows if r["best_shift_px"] == s) for s in ("-0.5", "+0.0", "+0.5")
            },
        },
    }


def table(rep: dict) -> str:
    lines = [
        "bundle            imgs  pts    mean   med    p90    signed(u,v)      pp_delta(u,v)    "
        "sweep(-.5/0/+.5)"
    ]
    for r in rep["bundles"]:
        sw = r["principal_point_sweep_mean_px"]
        lines.append(
            f"{r['bundle']:<17} {r['images']:>4} {r['points3D']:>5} "
            f"{r['reproj_mean_px']!s:>6} {r['reproj_median_px']!s:>6} {r['reproj_p90_px']!s:>6} "
            f"{r['mean_signed_residual_px']!s:>16} {r['refined_principal_point_delta_px']!s:>16} "
            f"{sw['-0.5']}/{sw['+0.0']}/{sw['+0.5']}"
        )
    return "\n".join(lines)
