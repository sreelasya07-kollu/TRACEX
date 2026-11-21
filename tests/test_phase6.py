"""Phase 6 (HANDOFF P6): SpatialMap writer/reader, schemas, determinism, end-to-end run.

light: fixture built from the synthetic room (no COLMAP): schema validity, manifest hashes,
       tamper detection, schema files up to date with the pydantic models.
heavy: real chain on Replica room0 via `run_pipeline`; deterministic files (everything downstream
       of Phase 2) hash-identical on a second run over the same workspace.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import cv2
import jsonschema
import numpy as np
import open3d as o3d
import pytest

from eval.synthetic_room import fused_cloud
from formats.models import CamerasFile, Intrinsics, PlanesFile, Priors, RoomModel, StageMeta
from formats.spatial_map import SpatialMapError, load_spatial_map, verify
from spatial.room.run import run_room
from spatial.spatial_map import quality_report, write_spatial_map

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "runs" / "test_results"
SPEC = ROOT / "spec"


def _meta(stage, metrics=None, params=None) -> str:
    return StageMeta(
        stage=stage,
        status="ok",
        started="2026-11-14T00:00:00+00:00",
        ended="2026-11-14T00:00:01+00:00",
        wall_time_s=1.0,
        device="cpu",
        peak_ram_mb=100.0,
        peak_vram_mb=None,
        versions={},
        params=params or {},
        inputs={},
        outputs={},
        metrics=metrics or {},
    ).model_dump_json()


@pytest.fixture(scope="module")
def fixture(tmp_path_factory):
    d = tmp_path_factory.mktemp("sm_fixture")
    ws, p2, p3, p5 = d / "ws", d / "phase2", d / "phase3", d / "phase5"
    for x in (ws / "sparse" / "0", p2, p3 / "depth_aligned", p3 / "depth_valid", p3 / "fusion"):
        x.mkdir(parents=True)
    T = np.eye(4).tolist()
    ids = ["000000", "000001"]
    pri = Priors(
        width=64,
        height=48,
        intrinsics=Intrinsics(fx=50, fy=50, cx=31.5, cy=23.5),
        frames=[
            {"id": i, "timestamp_ns": k, "image": f"images/{i}.jpg", "T_world_camera": T}
            for k, i in enumerate(ids)
        ],
        pose_source="dataset_gt_noisy",
        gravity_world=[0, 0, -1],
        gravity_source="dataset_axis",
        depth_source="rendered",
    )
    (ws / "priors.json").write_text(pri.model_dump_json())
    (ws / "alignment.json").write_text(
        json.dumps({"metric": True, "gravity_aligned": True, "prior_residual_p95_m": 0.01})
    )
    (ws / "poses_refined.json").write_text(json.dumps({i: T for i in ids}))
    (ws / "workspace_meta.json").write_text(json.dumps({"dataset": "fixture", "scene": "x"}))
    for n in ("cameras.bin", "images.bin", "points3D.bin"):
        (ws / "sparse" / "0" / n).write_bytes(n.encode())
    (ws / "images").mkdir()
    for i in ids:
        cv2.imwrite(str(ws / "images" / f"{i}.jpg"), np.zeros((48, 64, 3), np.uint8))
    (p2 / "meta.json").write_text(
        _meta(
            "phase2_refine",
            {"registered": 2, "n_frames": 2, "reg_ratio": 1.0, "mean_reproj_px": 0.5},
        )
    )
    for i in ids:
        cv2.imwrite(str(p3 / "depth_aligned" / f"{i}.png"), np.full((24, 32), 2000, np.uint16))
        cv2.imwrite(str(p3 / "depth_valid" / f"{i}.png"), np.full((24, 32), 255, np.uint8))
    (p3 / "depth_alignment.json").write_text("{}")
    o3d.io.write_point_cloud(str(p3 / "fusion" / "cloud.ply"), fused_cloud(seed=0))
    o3d.io.write_triangle_mesh(
        str(p3 / "fusion" / "tsdf_mesh.ply"), o3d.geometry.TriangleMesh.create_box()
    )
    (p3 / "meta.json").write_text(
        _meta(
            "phase3",
            {
                "frame_ids": ids,
                "width": 32,
                "height": 24,
                "valid_fraction": 0.9,
                "depth_source": "sensor",
                "n_points": 1000,
            },
        )
    )
    run_room(p3 / "fusion" / "cloud.ply", p5, scale_source="metric_priors")
    return d, ws, p2, p3, p5


@pytest.mark.light
def test_write_load_verify_and_tamper(fixture):
    d, ws, p2, p3, p5 = fixture
    out = d / "spatial_map"
    write_spatial_map(ws, p2, p3, p5, out, link=False)
    sm = load_spatial_map(out)
    assert sm.cameras.depth_width == 32 and sm.cameras.depth_intrinsics.fx == pytest.approx(25.0)
    assert sm.room.floor_polygon_source in ("wall_intersections", "footprint_contour")
    assert sm.quality.n_walls == 4 and sm.quality.warnings == []
    assert sm.manifest.files["fused_cloud.ply"] and verify(out) == []
    assert sorted(p.name for p in sm.images_dir.glob("*.jpg")) == ["000000.jpg", "000001.jpg"]
    # schema validation of every JSON file against spec/ (the published contract)
    for rel, schema in [
        ("manifest.json", "spatial_map/manifest"),
        ("cameras/cameras.json", "spatial_map/cameras"),
        ("planes.json", "spatial_map/planes"),
        ("room.json", "spatial_map/room"),
        ("quality.json", "spatial_map/quality"),
    ]:
        jsonschema.validate(
            json.loads((out / rel).read_text()),
            json.loads((SPEC / f"{schema}.schema.json").read_text()),
        )
    # tamper detection: corrupt one file, add a stray file, delete one
    shutil.copytree(out, d / "bad")
    (d / "bad" / "fused_cloud.ply").write_bytes(b"corrupt")
    (d / "bad" / "extra.txt").write_text("x")
    (d / "bad" / "planes.json").unlink()
    probs = verify(d / "bad")
    assert any("hash mismatch: fused_cloud.ply" in p for p in probs)
    assert any("not in manifest: extra.txt" in p for p in probs)
    assert any("missing: planes.json" in p for p in probs)
    with pytest.raises(SpatialMapError):
        load_spatial_map(d / "bad")
    # writer is deterministic: second write of the same stage outputs -> identical file hashes
    write_spatial_map(ws, p2, p3, p5, d / "again", link=False)
    assert load_spatial_map(d / "again").manifest.files == sm.manifest.files


@pytest.mark.light
def test_quality_warnings_flag_problems(fixture, tmp_path):
    d, ws, p2, p3, p5 = fixture
    ws2 = tmp_path / "ws"
    shutil.copytree(ws, ws2)
    (ws2 / "alignment.json").write_text(json.dumps({"metric": False, "gravity_aligned": False}))
    sm = load_spatial_map(write_spatial_map(ws2, p2, p3, p5, tmp_path / "sm", link=False))
    assert any("not gravity-aligned" in w for w in sm.quality.warnings)
    assert any("not metric" in w for w in sm.quality.warnings)


@pytest.mark.light
def test_quality_report_flags_split_sfm(fixture):
    """Issue #4: a split SfM reconstruction (several models, largest kept) is a warning."""
    d, ws, p2, p3, p5 = fixture
    cams = CamerasFile(
        width=64,
        height=48,
        intrinsics=Intrinsics(fx=50, fy=50, cx=31.5, cy=23.5),
        depth_width=32,
        depth_height=24,
        depth_intrinsics=Intrinsics(fx=25, fy=25, cx=15.5, cy=11.5),
        pose_source="dataset_gt_noisy",
        metric=True,
        gravity_aligned=True,
        frames={},
    )
    planes = PlanesFile.model_validate_json((p5 / "planes.json").read_text())
    room = RoomModel.model_validate_json((p5 / "room.json").read_text())
    p3m = StageMeta.model_validate_json((p3 / "meta.json").read_text()).metrics
    reg = {"registered": 20, "n_frames": 26, "reg_ratio": 20 / 26, "mean_reproj_px": 0.5}
    split = quality_report(cams, {}, p3m, planes, room, reg | {"model_sizes": [20, 6]})
    msg = "SfM split into 2 models (sizes 20, 6); kept the largest; largest model has 20/26 frames"
    assert msg in split.warnings
    for sizes in ([26], None):
        q = quality_report(cams, {}, p3m, planes, room, reg | {"model_sizes": sizes})
        assert not any("SfM split" in w for w in q.warnings)


@pytest.mark.light
def test_published_schemas_up_to_date():
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts/export_schemas.py"), "--check"],
        capture_output=True,
        text=True,
        cwd=ROOT,
        env={"PYTHONPATH": str(ROOT), "PATH": "/usr/bin:/bin"},
    )
    assert r.returncode == 0, r.stdout + r.stderr


REPLICA = ROOT / "data/replica/room0"


@pytest.mark.heavy
def test_pipeline_end_to_end_replica(tmp_path):
    if not (REPLICA / "SUBSET.json").exists():
        pytest.skip("dataset missing (data/fetch.sh)")
    from spatial.pipeline import run_pipeline

    sm_dir = run_pipeline(
        "replica",
        REPLICA,
        tmp_path / "run",
        max_frames=40,
        stride=10,
        downscale=2,
        max_image_size=640,
        depth_scale=True,
    )
    sm = load_spatial_map(sm_dir)  # verifies every hash
    meta = {k: v.model_dump() for k, v in sm.manifest.stages.items()}
    total = sum(v["wall_time_s"] for v in meta.values())
    out = {
        "stages": meta,
        "total_stage_wall_s": total,
        "peak_ram_mb": max(v["peak_ram_mb"] for v in meta.values()),
        "quality": sm.quality.model_dump(),
        "n_files": len(sm.manifest.files),
    }
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "phase6_end_to_end_replica.json").write_text(json.dumps(out, indent=1))
    assert sm.quality.reg_ratio >= 0.95
    assert sm.cameras.metric and sm.cameras.gravity_aligned
    assert sm.quality.n_walls >= 4 and sm.room.ceiling_height_m is not None
    assert sm.manifest.params["phase2"]["depth_scale"] is True


@pytest.mark.heavy
def test_downstream_of_phase2_is_deterministic(tmp_path):
    from spatial.fusion.tsdf import TSDFParams
    from spatial.phase3 import Phase3Config, run_phase3
    from spatial.stage import sha256_file

    ws = ROOT / "runs/bench_phase3/room0/_workspace/workspace"
    if not (ws / "poses_refined.json").exists():
        pytest.skip("needs benchmarks.phase3 workspace")
    cfg = Phase3Config(
        depth_source="sensor", downscale=2, max_frames=20, tsdf_p=TSDFParams(voxel=0.03)
    )
    hashes = []
    for k in range(2):
        out = tmp_path / f"r{k}"
        run_phase3(ws, out, cfg)
        run_room(out / "fusion" / "cloud.ply", out / "p5", scale_source="metric_priors")
        files = [
            out / "fusion" / "cloud.ply",
            out / "fusion" / "tsdf_mesh.ply",
            out / "p5" / "planes.json",
            out / "p5" / "room.json",
            *sorted((out / "depth_aligned").glob("*.png")),
        ]
        hashes.append({str(f.relative_to(out)): sha256_file(f) for f in files})
    same = hashes[0] == hashes[1]
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "phase6_determinism.json").write_text(
        json.dumps({"n_files": len(hashes[0]), "identical": same}, indent=1)
    )
    assert same
