"""QA gate / Job Report on synthetic SpatialMaps with exact ground truth."""

from __future__ import annotations

import json
from pathlib import Path

import jsonschema
import numpy as np
import pytest

from eval.fixtures import build_synthetic_spatial_map
from formats.models import OutputEntry, ReconstructionManifest
from formats.spatial_map import load_spatial_map
from formats.util import sha256_file
from qa.gates import build_report
from qa.thresholds import Thresholds

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def maps(tmp_path_factory):
    d = tmp_path_factory.mktemp("qa")
    return {
        "box": load_spatial_map(build_synthetic_spatial_map(d / "box", "box")),
        "L": load_spatial_map(build_synthetic_spatial_map(d / "L", "L")),
        # cameras that look towards +x are not registered: the x = 4 m wall is never observed
        "gap": load_spatial_map(
            build_synthetic_spatial_map(d / "gap", "box", keep_camera=lambda T: T[0, 2] < 0.2)
        ),
    }


def _status(r, name):
    return next(g.status for g in r.gates if g.name == name)


@pytest.mark.light
@pytest.mark.parametrize("layout", ["box", "L"])
def test_fully_covered_room_passes(maps, layout):
    r = build_report(maps[layout])
    assert r.verdict in ("pass", "pass_with_warnings"), [g for g in r.gates if g.status == "fail"]
    assert r.coverage["wall"] >= 0.95 and r.coverage["floor"] >= 0.9
    assert not [g for g in r.gates if g.status == "fail"]
    # the L-shaped room's notch corner is seen from nearly one position only: a small, honestly
    # reported residual region (< 1 m2 in total), never a failed gate
    assert sum(x.area_m2 for x in r.rescan) < 1.0


@pytest.mark.light
def test_unobserved_wall_gives_anchored_rescan_instruction(maps):
    r = build_report(maps["gap"])
    assert r.verdict == "needs_rescan" and _status(r, "wall_coverage") == "fail"
    top = r.rescan[0]
    assert top.surface == "wall" and top.priority == max(x.priority for x in r.rescan)
    # anchored in the world frame: the region lies on the x = 4 m wall (GT), not just "somewhere"
    c = np.asarray(top.corners)
    assert np.all(np.abs(c[:, 0] - 4.0) < 0.1) and c[:, 2].max() > 2.0 and top.area_m2 > 5
    assert "facing -X" in top.instruction and "above the floor" in top.instruction
    # ground truth: the wall that is really unobserved is the one with normal -x (x = 4 m)
    assert maps["gap"].room.floor_polygon[1][0] == pytest.approx(4.0, abs=0.05)


@pytest.mark.light
@pytest.mark.parametrize(
    "update,gate,expected,verdict",
    [
        ({"quality": {"reg_ratio": 0.5, "registered": 5}}, "registration", "fail", "needs_rescan"),
        ({"quality": {"reg_ratio": 0.9, "registered": 9}}, "registration", "warn", None),
        ({"quality": {"mean_reproj_px": 3.0}}, "reprojection_error", "fail", "needs_rescan"),
        ({"quality": {"depth_valid_fraction": 0.1}}, "depth_validity", "fail", "needs_rescan"),
        ({"quality": {"wall_floor_max_dev_deg": 4.0}}, "wall_floor_perpendicular", "warn", None),
        ({"room": {"scale_confidence": 0.2}}, "scale_confidence", "fail", "needs_rescan"),
        ({"room": {"ceiling_height_m": 8.0}}, "ceiling_height_plausible", "fail", "needs_rescan"),
        ({"room": {"floor_id": None}}, "floor_found", "fail", "needs_rescan"),
        ({"room": {"ceiling_id": None}}, "ceiling_found", "warn", None),
        ({"room": {"floor_polygon_source": "floor_hull"}}, "closed_room_outline", "warn", None),
        ({"cameras": {"gravity_aligned": False}}, "gravity_alignment", "fail", "needs_rescan"),
        ({"cameras": {"metric": False}}, "metric_scale", "fail", "needs_rescan"),
    ],
)
def test_gate_logic(maps, update, gate, expected, verdict):
    sm = maps["box"]
    sm2 = sm.__class__(
        **{
            **{
                f: getattr(sm, f)
                for f in ("root", "manifest", "cameras", "planes", "room", "quality")
            },
            **{k: getattr(sm, k).model_copy(update=v) for k, v in update.items()},
        }
    )
    r = build_report(sm2)
    assert _status(r, gate) == expected
    if verdict:
        assert r.verdict == verdict
    else:
        assert r.verdict == "pass_with_warnings"


def _recon(tmp: Path, mesh_psnr: float | None, splat: OutputEntry) -> Path:
    tmp.mkdir(parents=True)
    ho = None if mesh_psnr is None else {"mean_psnr_covered": mesh_psnr, "mean_coverage": 0.4}
    man = ReconstructionManifest(
        created="2026-11-14T00:00:00+00:00",
        spatial_map={"path": "x", "manifest_sha256": "0" * 64},
        outputs={
            "sparse": OutputEntry(status="ok"),
            "mesh": OutputEntry(
                status="ok", metrics={"mesh_faces": 10, **({"heldout": ho} if ho else {})}
            ),
            "splat": splat,
        },
        files={},
        timings_s={},
        peak_ram_mb=1.0,
        params={},
        versions={},
    )
    (tmp / "manifest.json").write_text(man.model_dump_json())
    return tmp


@pytest.mark.light
def test_output_gates_do_not_trigger_rescan_but_never_hide_failures(maps, tmp_path):
    skipped = OutputEntry(status="skipped", reason="no CUDA device")
    r = build_report(maps["box"], _recon(tmp_path / "a", 8.8, skipped))
    assert _status(r, "mesh_heldout_psnr") == "fail"  # reported honestly ...
    assert _status(r, "splat_quality") == "skipped"  # ... a skipped splat is not a pass
    assert r.verdict == "pass_with_warnings"  # ... but a rescan cannot fix it
    r2 = build_report(maps["box"], _recon(tmp_path / "b", 20.0, skipped))
    assert _status(r2, "mesh_heldout_psnr") == "pass"
    good = OutputEntry(status="ok", metrics={"psnr": 25.0, "ssim": 0.85, "lpips": 0.2})
    assert (
        _status(build_report(maps["box"], _recon(tmp_path / "c", 20.0, good)), "splat_quality")
        == "pass"
    )
    bad = OutputEntry(status="ok", metrics={"psnr": 20.0, "ssim": 0.85, "lpips": 0.2})
    assert (
        _status(build_report(maps["box"], _recon(tmp_path / "d", 20.0, bad)), "splat_quality")
        == "fail"
    )


@pytest.mark.light
def test_report_schema_cli_and_thresholds_object(maps, tmp_path):
    from typer.testing import CliRunner

    from qa.cli import app

    out = tmp_path / "job_report.json"
    res = CliRunner().invoke(
        app, ["report", "--spatial-map", str(maps["gap"].root), "--out", str(out)]
    )
    assert res.exit_code == 0, res.output
    data = json.loads(out.read_text())
    jsonschema.validate(data, json.loads((ROOT / "spec/qa/job_report.schema.json").read_text()))
    assert json.loads(res.output.splitlines()[-1])["verdict"] == "needs_rescan"
    assert data["inputs"]["spatial_map_manifest_sha256"] == sha256_file(
        maps["gap"].root / "manifest.json"
    )
    # thresholds are data, not code: a laxer wall threshold turns the same scan into a warning
    lax = Thresholds(wall_coverage_pass=0.5, wall_coverage_warn=0.5)
    assert _status(build_report(maps["gap"], th=lax), "wall_coverage") == "pass"
