"""Phase 5 stage: fused cloud -> planes.json + room.json (+ meta.json). docs/INTERFACES.md §4."""

from __future__ import annotations

from dataclasses import asdict
from pathlib import Path

import numpy as np
import open3d as o3d

from formats.models import Plane, PlanesFile, RoomModel
from spatial.geometry import rotation_aligning
from spatial.room.planes import PlaneParams, detect_planes, floor_polygon, label_planes
from spatial.stage import record_stage

_SCALE_BASE = {"synthetic": 1.0, "sensor_depth": 0.9, "metric_priors": 0.7, "unknown": 0.0}


def scale_confidence(scale_source: str, alignment: dict | None = None) -> float:
    """HEURISTIC in [0, 1], not a calibrated probability. Base value by where the metric scale
    comes from (sensor depth 0.9, metric pose priors 0.7, unknown 0). Reduced by 0.2 when the
    pose priors disagree with the SfM trajectory (p95 Sim(3) residual > 5 cm), and by 0.1 when
    the depth-derived scale has a per-image spread (MAD / scale) above 2 %."""
    c = _SCALE_BASE.get(scale_source, 0.0)
    if alignment:
        if alignment.get("prior_residual_p95_m", 0.0) > 0.05 and scale_source == "metric_priors":
            c -= 0.2
        ds = alignment.get("depth_scale") or {}
        if ds.get("status") == "ok" and ds["mad"] / ds["scale"] > 0.02:
            c -= 0.1
    return float(np.clip(c, 0.0, 1.0))


def run_room(
    cloud: Path,
    out: Path,
    p: PlaneParams | None = None,
    scale_source: str = "unknown",
    alignment: dict | None = None,
) -> dict:
    """`cloud`: fused cloud in the gravity-aligned world frame (+Z up, metres)."""
    p = p or PlaneParams()
    with record_stage("phase5_room", out, device="cpu", params=asdict(p)) as rec:
        rec.add_input(cloud)
        pcd = o3d.io.read_point_cloud(str(cloud))
        if len(pcd.points) == 0:
            raise ValueError(f"empty or unreadable cloud: {cloud}")
        P, planes = detect_planes(pcd, p)
        lab = label_planes(P, planes, p)
        poly, area, src = floor_polygon(P, planes, lab, p)

        out_planes = []
        for k, pl in enumerate(planes):
            polygon, a = pl.hull(P)
            out_planes.append(
                Plane(
                    id=k,
                    label=pl.label,
                    normal=pl.normal.tolist(),
                    d=pl.d,
                    inlier_count=len(pl.idx),
                    area_m2=a,
                    rms_m=pl.rms(P),
                    polygon=polygon,
                )
            )
        n_assigned = int(sum(len(pl.idx) for pl in planes))
        pf = PlanesFile(planes=out_planes, n_points=len(P), n_assigned=n_assigned, params=asdict(p))
        notes = []
        if lab["floor"] is None:
            notes.append("no floor plane found")
        if lab["ceiling"] is None:
            notes.append("no ceiling plane found (ceiling unseen or not planar)")
        if src != "wall_intersections":
            notes.append(f"floor polygon from {src}: wall loop not closed or failed validation")
        fz, cz = lab["floor_z"], lab["ceiling_z"]
        tilt, R_up = None, None
        if lab["floor"] is not None:
            nf = planes[lab["floor"]].normal
            tilt = float(np.degrees(np.arccos(np.clip(nf[2], -1, 1))))
            R_up = rotation_aligning(nf, np.array([0.0, 0.0, 1.0])).tolist()
        room = RoomModel(
            floor_polygon=poly.tolist(),
            floor_polygon_source=src,
            floor_area_m2=area,
            floor_z=fz,
            ceiling_z=cz,
            ceiling_height_m=lab["ceiling_height"],
            floor_id=lab["floor"],
            ceiling_id=lab["ceiling"],
            wall_ids=lab["walls"],
            floor_tilt_deg=tilt,
            R_floor_up=R_up,
            bbox_min=P.min(0).tolist(),
            bbox_max=P.max(0).tolist(),
            gravity_world=[0.0, 0.0, -1.0],
            scale_source=scale_source,
            scale_confidence=scale_confidence(scale_source, alignment),
            notes=notes,
        )
        (out / "planes.json").write_text(pf.model_dump_json(indent=1))
        (out / "room.json").write_text(room.model_dump_json(indent=1))
        rec.add_output(out / "planes.json")
        rec.add_output(out / "room.json")
        rec.metrics |= {
            "n_planes": len(planes),
            "labels": {
                lbl: sum(q.label == lbl for q in planes)
                for lbl in ("floor", "ceiling", "wall", "other")
            },
            "assigned_fraction": n_assigned / max(1, len(P)),
            "floor_polygon_source": src,
            "floor_area_m2": area,
            "ceiling_height_m": room.ceiling_height_m,
            "n_corners": len(poly),
            "floor_tilt_deg": (
                float(np.degrees(np.arccos(min(1.0, abs(planes[lab["floor"]].normal[2])))))
                if lab["floor"] is not None
                else None
            ),
        }
        rec.notes += notes
        return {"planes": planes, "P": P, "room": room, "metrics": rec.metrics}


def wall_floor_angles_deg(planes: list[Plane]) -> list[float]:
    """|90° - angle(floor normal, wall normal)| per wall (0 = perfectly perpendicular)."""
    fl = [q for q in planes if q.label == "floor"]
    if not fl:
        return []
    nf = np.asarray(fl[0].normal)
    return [
        float(abs(90.0 - np.degrees(np.arccos(np.clip(abs(nf @ np.asarray(w.normal)), 0, 1)))))
        for w in planes
        if w.label == "wall"
    ]
