"""Phase 5: planes + room (HANDOFF §5 P5). Measured values -> runs/test_results/phase5_*.json.

light: synthetic room with exact GT (eval/synthetic_room.py), schema + determinism.
heavy: Replica room0 Phase 3 sensor-depth cloud (runs/bench_phase3/room0/R1): wall ⟂ floor,
       and ceiling height / floor area vs the same detector run on the GT mesh.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import open3d as o3d
import pytest

from eval.synthetic_room import fused_cloud, gt
from formats.models import PlanesFile, RoomModel
from spatial.room.run import run_room, wall_floor_angles_deg
from spatial.stage import sha256_file

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "runs" / "test_results"


def _save(name, values):
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / f"phase5_{name}.json").write_text(json.dumps(values, indent=1, default=str))


@pytest.fixture(scope="module")
def synthetic(tmp_path_factory):
    d = tmp_path_factory.mktemp("p5_syn")
    o3d.io.write_point_cloud(str(d / "cloud.ply"), fused_cloud(seed=0))
    r = run_room(d / "cloud.ply", d / "phase5", scale_source="synthetic")
    return d, r


def _match(gtp, planes, label):
    """Detected plane with `label`, normal within 10° of GT, nearest to the GT plane point."""
    n_gt, c = np.asarray(gtp["normal"]), np.asarray(gtp["point"])
    cands = [q for q in planes if q.label == label and q.normal @ n_gt > np.cos(np.radians(10))]
    if not cands:
        return None
    q = min(cands, key=lambda q: abs(q.normal @ c + q.d))
    return {
        "normal_err_deg": float(np.degrees(np.arccos(np.clip(q.normal @ n_gt, -1, 1)))),
        "offset_err_m": float(abs(q.normal @ c + q.d)),
    }


@pytest.mark.light
def test_synthetic_room_matches_gt(synthetic):
    d, r = synthetic
    G, room = gt(), r["room"]
    per = {
        k: _match(v, r["planes"], k.split("_")[0]) for k, v in G["planes"].items()
    }  # floor, ceiling, wall_*
    poly = np.asarray(room.floor_polygon)
    corner_err = [
        float(np.min(np.linalg.norm(poly - np.asarray(c), axis=1))) for c in G["corners_xy"]
    ]
    vals = {
        "planes": per,
        "ceiling_height_m": room.ceiling_height_m,
        "ceiling_height_err_m": abs(room.ceiling_height_m - G["ceiling_height_m"]),
        "floor_area_m2": room.floor_area_m2,
        "floor_area_rel_err": abs(room.floor_area_m2 - G["floor_area_m2"]) / G["floor_area_m2"],
        "floor_polygon_source": room.floor_polygon_source,
        "corner_err_m": corner_err,
        "labels": r["metrics"]["labels"],
        "wall_time_s": json.loads((d / "phase5" / "meta.json").read_text())["wall_time_s"],
    }
    _save("synthetic", vals)
    assert all(per.values()), f"GT plane not detected: {per}"
    assert max(v["normal_err_deg"] for v in per.values()) <= 2.0
    assert max(v["offset_err_m"] for v in per.values()) <= 0.02
    assert vals["ceiling_height_err_m"] <= 0.02
    assert vals["floor_area_rel_err"] <= 0.03
    assert room.floor_polygon_source == "wall_intersections" and len(poly) == 4
    assert r["metrics"]["labels"]["wall"] == 4  # cabinet/table faces are not walls


@pytest.mark.light
def test_outputs_schema_valid_and_deterministic(synthetic, tmp_path):
    d, _ = synthetic
    PlanesFile.model_validate_json((d / "phase5" / "planes.json").read_text())
    RoomModel.model_validate_json((d / "phase5" / "room.json").read_text())
    run_room(d / "cloud.ply", tmp_path / "again", scale_source="synthetic")
    for f in ("planes.json", "room.json"):
        assert sha256_file(d / "phase5" / f) == sha256_file(tmp_path / "again" / f), f


def _polygon_iou(a: np.ndarray, b: np.ndarray, cell: float = 0.02) -> float:
    import cv2

    lo = np.minimum(a.min(0), b.min(0)) - 0.1
    hi = np.maximum(a.max(0), b.max(0)) + 0.1
    shape = tuple(np.ceil((hi - lo)[::-1] / cell).astype(int))

    def raster(poly):
        m = np.zeros(shape, np.uint8)
        cv2.fillPoly(m, [np.round((poly - lo) / cell).astype(np.int32).reshape(-1, 1, 2)], 1)
        return m.astype(bool)

    ra, rb = raster(a), raster(b)
    return float((ra & rb).sum() / max(1, (ra | rb).sum()))


REPLICA_CLOUD = ROOT / "runs/bench_phase3/room0/R1/phase3/fusion/cloud.ply"
REPLICA_MESH = ROOT / "data/replica/room0_mesh.ply"


@pytest.mark.heavy
def test_replica_room0_real_cloud(tmp_path):
    if not (REPLICA_CLOUD.exists() and REPLICA_MESH.exists()):
        pytest.skip("needs Phase 3 bench run R1 (benchmarks.phase3) and data/replica")
    from eval.gt import load_mesh
    from eval.recon_metrics import sample_mesh

    r = run_room(REPLICA_CLOUD, tmp_path / "pred", scale_source="metric_priors")
    g = sample_mesh(load_mesh(REPLICA_MESH), 400000, seed=0)
    g.estimate_normals(o3d.geometry.KDTreeSearchParamHybrid(0.09, 30))
    o3d.io.write_point_cloud(str(tmp_path / "gt.ply"), g)
    rg = run_room(tmp_path / "gt.ply", tmp_path / "gt", scale_source="synthetic")
    pf = PlanesFile.model_validate_json((tmp_path / "pred" / "planes.json").read_text())
    dev = wall_floor_angles_deg(pf.planes)
    p, q = r["room"], rg["room"]
    vals = {
        "wall_floor_deviation_deg": dev,
        "n_walls": len(dev),
        "floor_tilt_deg": r["metrics"]["floor_tilt_deg"],
        "ceiling_height_m": p.ceiling_height_m,
        "ceiling_height_gtmesh_m": q.ceiling_height_m,
        "floor_area_m2": p.floor_area_m2,
        "floor_area_gtmesh_m2": q.floor_area_m2,
        "floor_polygon_source": [p.floor_polygon_source, q.floor_polygon_source],
        "wall_time_s": json.loads((tmp_path / "pred" / "meta.json").read_text())["wall_time_s"],
        "peak_ram_mb": json.loads((tmp_path / "pred" / "meta.json").read_text())["peak_ram_mb"],
    }
    _save("replica_room0", vals)
    assert len(dev) >= 4 and max(dev) <= 3.0  # HANDOFF P5 real-data check: floor ⟂ wall
    assert abs(p.ceiling_height_m - q.ceiling_height_m) <= 0.02
    assert abs(p.floor_area_m2 - q.floor_area_m2) / q.floor_area_m2 <= 0.03
    # self-consistency: predicted footprint polygon vs the polygon from the GT-mesh cloud
    iou = _polygon_iou(np.asarray(p.floor_polygon), np.asarray(q.floor_polygon))
    _save("replica_room0", {**vals, "floor_polygon_iou_gtmesh": iou})
    assert iou >= 0.95


@pytest.mark.light
def test_l_shaped_room_polygon_and_fields(tmp_path):
    """A 4x3.5 m box with a 1.5x1.5 m corner removed: one-line-per-direction walls cannot
    describe it; the footprint contour must (area <= 3 % off, every GT corner within 8 cm)."""
    from eval.synthetic_room import L_AREA, L_OUTLINE

    o3d.io.write_point_cloud(str(tmp_path / "l.ply"), fused_cloud(seed=0, layout="L"))
    r = run_room(tmp_path / "l.ply", tmp_path / "p5", scale_source="synthetic")
    room = r["room"]
    poly = np.asarray(room.floor_polygon)
    corner_err = [float(np.min(np.linalg.norm(poly - np.asarray(c), axis=1))) for c in L_OUTLINE]
    vals = {
        "source": room.floor_polygon_source,
        "n_corners": len(poly),
        "area_m2": room.floor_area_m2,
        "area_rel_err": abs(room.floor_area_m2 - L_AREA) / L_AREA,
        "corner_err_m": corner_err,
        "n_walls": r["metrics"]["labels"]["wall"],
        "floor_tilt_deg": room.floor_tilt_deg,
        "scale_confidence": room.scale_confidence,
    }
    _save("synthetic_L", vals)
    assert room.floor_polygon_source == "footprint_contour"
    # wall-line snapping: max corner error 4.5 cm -> 1.0 cm (measured); 2 cm leaves headroom
    assert vals["area_rel_err"] <= 0.03 and max(corner_err) <= 0.02
    assert vals["n_walls"] == 6
    assert room.floor_tilt_deg is not None and room.floor_tilt_deg < 1.0
    assert np.allclose(np.asarray(room.R_floor_up) @ np.asarray(room.R_floor_up).T, np.eye(3))


@pytest.mark.light
def test_recess_room_polygon(tmp_path):
    """4x3.5 m box with a 0.8x0.4 m recess (2.8 % of the area, below the old IoU gate): the
    floor polygon must still contain it (8 corners, every GT corner within 8 cm)."""
    from eval.synthetic_room import RECESS_AREA, RECESS_OUTLINE

    o3d.io.write_point_cloud(str(tmp_path / "r.ply"), fused_cloud(seed=0, layout="recess"))
    r = run_room(tmp_path / "r.ply", tmp_path / "p5", scale_source="synthetic")
    room = r["room"]
    poly = np.asarray(room.floor_polygon)
    corner_err = [
        float(np.min(np.linalg.norm(poly - np.asarray(c), axis=1))) for c in RECESS_OUTLINE
    ]
    vals = {
        "source": room.floor_polygon_source,
        "n_corners": len(poly),
        "area_m2": room.floor_area_m2,
        "area_rel_err": abs(room.floor_area_m2 - RECESS_AREA) / RECESS_AREA,
        "corner_err_m": corner_err,
        "n_walls": r["metrics"]["labels"]["wall"],
    }
    _save("synthetic_recess", vals)
    assert room.floor_polygon_source == "footprint_contour"
    assert vals["area_rel_err"] <= 0.03 and max(corner_err) <= 0.08
    assert vals["n_walls"] == 7


@pytest.mark.light
def test_scale_confidence_heuristic():
    from spatial.room.run import scale_confidence

    assert scale_confidence("unknown") == 0.0
    assert scale_confidence("sensor_depth") > scale_confidence("metric_priors") > 0
    assert scale_confidence("metric_priors", {"prior_residual_p95_m": 0.2}) < scale_confidence(
        "metric_priors", {"prior_residual_p95_m": 0.01}
    )
