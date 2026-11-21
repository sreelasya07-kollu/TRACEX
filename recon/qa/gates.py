"""Gates: deterministic checks of a SpatialMap (and optionally a Reconstruction) -> Job Report.

`capture` gates describe problems a rescan can fix; `output` gates describe processing quality.
Verdict: needs_rescan if any capture gate fails; else pass_with_warnings if anything warns or an
output gate fails; else pass. A skipped gate (e.g. splat not trained) is never a pass."""

from __future__ import annotations

import numpy as np

from formats.models import GateResult, JobReport, RescanRegion
from formats.reconstruction import load_reconstruction
from formats.spatial_map import SpatialMap
from formats.util import sha256_file
from qa.coverage import analyse
from qa.thresholds import Thresholds


def _grade(value, pass_ok: bool, warn_ok: bool) -> str:
    return "pass" if pass_ok else ("warn" if warn_ok else "fail")


def _g(name, category, status, value, threshold, message) -> GateResult:
    v = float(value) if isinstance(value, (int, float, np.floating)) else value
    return GateResult(
        name=name, category=category, status=status, value=v, threshold=threshold, message=message
    )


def spatial_gates(sm: SpatialMap, th: Thresholds, coverage: dict[str, float]) -> list[GateResult]:
    q, room, cams = sm.quality, sm.room, sm.cameras
    G: list[GateResult] = []
    s = _grade(q.reg_ratio, q.reg_ratio >= th.reg_ratio_pass, q.reg_ratio >= th.reg_ratio_warn)
    G.append(
        _g(
            "registration",
            "capture",
            s,
            q.reg_ratio,
            f">= {th.reg_ratio_pass}",
            f"{q.registered}/{q.n_frames} frames registered",
        )
    )
    if q.mean_reproj_px is not None:
        s = _grade(
            q.mean_reproj_px,
            q.mean_reproj_px <= th.reproj_pass_px,
            q.mean_reproj_px <= th.reproj_warn_px,
        )
        G.append(
            _g(
                "reprojection_error",
                "capture",
                s,
                q.mean_reproj_px,
                f"<= {th.reproj_pass_px} px",
                "mean reprojection error of the sparse model",
            )
        )
    G.append(
        _g(
            "gravity_alignment",
            "capture",
            "pass" if cams.gravity_aligned else "fail",
            str(cams.gravity_aligned),
            "aligned",
            "room labels need a gravity-aligned frame (device gravity / IMU missing)"
            if not cams.gravity_aligned
            else "world frame is gravity-aligned",
        )
    )
    G.append(
        _g(
            "metric_scale",
            "capture",
            "pass" if cams.metric else "fail",
            str(cams.metric),
            "metric",
            "model is not metric: use a depth/LiDAR device or place a reference"
            if not cams.metric
            else "model is metric",
        )
    )
    sc = room.scale_confidence
    if sc is not None:
        s = _grade(sc, sc >= th.scale_conf_pass, sc >= th.scale_conf_warn)
        G.append(
            _g(
                "scale_confidence",
                "capture",
                s,
                sc,
                f">= {th.scale_conf_pass}",
                f"heuristic scale trust (source: {room.scale_source})",
            )
        )
    G.append(
        _g(
            "floor_found",
            "capture",
            "pass" if room.floor_id is not None else "fail",
            str(room.floor_id is not None),
            "found",
            "floor plane detection",
        )
    )
    G.append(
        _g(
            "ceiling_found",
            "capture",
            "pass" if room.ceiling_id is not None else "warn",
            str(room.ceiling_id is not None),
            "found",
            "ceiling plane detection",
        )
    )
    nw = len(room.wall_ids)
    G.append(
        _g(
            "walls_found",
            "capture",
            "pass" if nw >= th.min_walls else "fail",
            nw,
            f">= {th.min_walls}",
            "wall planes detected",
        )
    )
    if q.wall_floor_max_dev_deg is not None:
        d = q.wall_floor_max_dev_deg
        s = _grade(d, d <= th.wall_floor_dev_pass_deg, d <= th.wall_floor_dev_warn_deg)
        G.append(
            _g(
                "wall_floor_perpendicular",
                "capture",
                s,
                d,
                f"<= {th.wall_floor_dev_pass_deg} deg",
                "worst wall vs floor deviation",
            )
        )
    rms = [p.rms_m for p in sm.planes.planes if p.label == "wall"]
    if rms:
        m = float(np.median(rms))
        s = _grade(m, m <= th.wall_rms_pass_m, m <= th.wall_rms_warn_m)
        G.append(
            _g(
                "wall_planarity",
                "capture",
                s,
                m,
                f"<= {th.wall_rms_pass_m} m",
                "median RMS distance of wall points to their plane",
            )
        )
    closed = room.floor_polygon_source in ("wall_intersections", "footprint_contour")
    G.append(
        _g(
            "closed_room_outline",
            "capture",
            "pass" if closed else ("warn" if room.floor_polygon_source == "floor_hull" else "fail"),
            room.floor_polygon_source,
            "closed wall loop",
            "how the floor outline was obtained",
        )
    )
    h = room.ceiling_height_m
    if h is not None:
        lo, hi = th.ceiling_height_pass_m
        wl, wh = th.ceiling_height_warn_m
        s = _grade(h, lo <= h <= hi, wl <= h <= wh)
        G.append(
            _g(
                "ceiling_height_plausible",
                "capture",
                s,
                h,
                f"{lo}-{hi} m",
                "implausible heights indicate a scale or plane-labelling error",
            )
        )
    s = _grade(
        q.depth_valid_fraction,
        q.depth_valid_fraction >= th.depth_valid_pass,
        q.depth_valid_fraction >= th.depth_valid_warn,
    )
    G.append(
        _g(
            "depth_validity",
            "capture",
            s,
            q.depth_valid_fraction,
            f">= {th.depth_valid_pass}",
            "fraction of depth pixels surviving filtering (glass, blank walls, motion lower it)",
        )
    )
    cw, cf, cc = coverage["wall"], coverage["floor"], coverage["ceiling"]
    G.append(
        _g(
            "wall_coverage",
            "capture",
            _grade(cw, cw >= th.wall_coverage_pass, cw >= th.wall_coverage_warn),
            cw,
            f">= {th.wall_coverage_pass}",
            "observed fraction of expected wall area",
        )
    )
    G.append(
        _g(
            "floor_coverage",
            "capture",
            _grade(cf, cf >= th.floor_coverage_pass, cf >= th.floor_coverage_warn),
            cf,
            f">= {th.floor_coverage_pass}",
            "observed fraction of floor area",
        )
    )
    G.append(
        _g(
            "ceiling_coverage",
            "capture",
            "pass" if cc >= th.ceiling_coverage_warn else "warn",
            cc,
            f">= {th.ceiling_coverage_warn}",
            "observed fraction of ceiling area",
        )
    )
    return G


def output_gates(recon_root, th: Thresholds) -> list[GateResult]:
    man = load_reconstruction(recon_root)
    G: list[GateResult] = []
    mesh = man.outputs.get("mesh")
    if mesh is None or mesh.status != "ok":
        G.append(
            _g(
                "mesh",
                "output",
                "fail" if mesh else "skipped",
                None,
                "mesh produced",
                (mesh.reason if mesh else "no mesh entry") or "mesh not produced",
            )
        )
    else:
        ho = mesh.metrics.get("heldout")
        if ho:
            v = ho["mean_psnr_covered"]
            G.append(
                _g(
                    "mesh_heldout_psnr",
                    "output",
                    _grade(v, v >= th.mesh_psnr_pass_db, v >= th.mesh_psnr_warn_db),
                    v,
                    f">= {th.mesh_psnr_pass_db} dB",
                    f"held-out view PSNR over covered pixels (coverage {ho['mean_coverage']:.2f})",
                )
            )
        else:
            G.append(
                _g(
                    "mesh_heldout_psnr",
                    "output",
                    "skipped",
                    None,
                    "evaluated",
                    "no held-out images were evaluated",
                )
            )
    sp = man.outputs.get("splat")
    if sp is None or sp.status != "ok":
        G.append(
            _g(
                "splat_quality",
                "output",
                "skipped",
                None,
                "trained",
                (sp.reason if sp else None) or "splat not produced",
            )
        )
    else:
        m = sp.metrics
        ok = (
            m["psnr"] >= th.splat_psnr
            and m["ssim"] >= th.splat_ssim
            and m["lpips"] <= th.splat_lpips
        )
        G.append(
            _g(
                "splat_quality",
                "output",
                "pass" if ok else "fail",
                m["psnr"],
                f"PSNR>={th.splat_psnr}, SSIM>={th.splat_ssim}, LPIPS<={th.splat_lpips}",
                f"held-out PSNR {m['psnr']:.1f}, SSIM {m['ssim']:.2f}, LPIPS {m['lpips']:.2f}",
            )
        )
    return G


def verdict(gates: list[GateResult]) -> str:
    if any(g.category == "capture" and g.status == "fail" for g in gates):
        return "needs_rescan"
    if any(g.status == "warn" or (g.category == "output" and g.status == "fail") for g in gates):
        return "pass_with_warnings"
    return "pass"


def build_report(sm: SpatialMap, recon_root=None, th: Thresholds | None = None) -> JobReport:
    th = th or Thresholds()
    coverage, regions = analyse(sm, th)
    gates = spatial_gates(sm, th, coverage)
    notes = ["coverage ignores occlusion: furniture can hide surfaces this model counts as seen"]
    if not sm.room.floor_polygon:
        notes.append("no floor polygon: surface coverage could not be computed")
    if recon_root is not None:
        gates += output_gates(recon_root, th)
    regions_out: list[RescanRegion] = regions
    return JobReport(
        verdict=verdict(gates),
        gates=gates,
        coverage=coverage,
        rescan=regions_out,
        inputs={
            "spatial_map": str(sm.root),
            "spatial_map_manifest_sha256": sha256_file(sm.root / "manifest.json"),
            "reconstruction": None if recon_root is None else str(recon_root),
        },
        notes=notes,
    )
