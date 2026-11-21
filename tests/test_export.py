"""Exports from a synthetic SpatialMap with exact ground truth (box and L-shaped room):
DXF/SVG/USD/LAZ round-trips against known dimensions."""

from __future__ import annotations

import json
import re

import numpy as np
import pytest

from eval.fixtures import build_synthetic_spatial_map
from eval.synthetic_room import L_AREA, L_OUTLINE
from export.floorplan import measurements, read_dxf_outline, write_dxf, write_svg
from export.geometry import area, triangulate
from export.pointcloud import read_laz_xyz, write_laz
from export.usd import room_shell_mesh, write_usda
from formats.spatial_map import load_spatial_map


@pytest.fixture(scope="module")
def maps(tmp_path_factory):
    d = tmp_path_factory.mktemp("exp")
    return {
        "box": load_spatial_map(build_synthetic_spatial_map(d / "box", "box")),
        "L": load_spatial_map(build_synthetic_spatial_map(d / "L", "L")),
    }


@pytest.mark.light
def test_ear_clipping_covers_l_shape_exactly():
    poly = np.array(L_OUTLINE, float)
    tris = triangulate(poly)
    assert len(tris) == len(poly) - 2
    assert sum(abs(area(poly[list(t)])) for t in tris) == pytest.approx(L_AREA, rel=1e-9)


@pytest.mark.light
@pytest.mark.parametrize("layout,gt_area", [("box", 14.0), ("L", L_AREA)])
def test_floorplan_dxf_svg_measurements(maps, tmp_path, layout, gt_area):
    sm = maps[layout]
    write_dxf(sm, tmp_path / "p.dxf")
    write_svg(sm, tmp_path / "p.svg")
    outline = read_dxf_outline(tmp_path / "p.dxf")
    assert len(outline) == len(sm.room.floor_polygon)
    np.testing.assert_allclose(outline, sm.room.floor_polygon, atol=1e-9)
    assert abs(area(outline) - gt_area) / gt_area < 0.03  # exported area vs exact GT
    svg = (tmp_path / "p.svg").read_text()
    assert svg.count("<polygon") == 1 and len(re.findall(r">[0-9.]+ m</text>", svg)) == len(outline)
    m = measurements(sm)
    assert (
        abs(m["ceiling_height_m"] - 2.6) < 0.02
        and abs(sum(m["wall_lengths_m"]) - m["perimeter_m"]) < 1e-9
    )
    assert abs(m["volume_m3"] - gt_area * 2.6) / (gt_area * 2.6) < 0.04


@pytest.mark.light
def test_usd_shell_is_consistent_and_watertight(maps, tmp_path):
    sm = maps["L"]
    pts, counts, idx = room_shell_mesh(sm)
    assert sum(counts) == len(idx) and max(idx) < len(pts)
    # closed surface: every undirected edge is shared by exactly two faces
    edges: dict[tuple[int, int], int] = {}
    k = 0
    for c in counts:
        face = idx[k : k + c]
        k += c
        for a, b in zip(face, face[1:] + face[:1], strict=True):
            edges[tuple(sorted((a, b)))] = edges.get(tuple(sorted((a, b))), 0) + 1
    assert set(edges.values()) == {2}
    # consistent orientation: every directed edge appears exactly once (its reverse is the twin)
    directed, k = [], 0
    for c in counts:
        face = idx[k : k + c]
        k += c
        directed += list(zip(face, face[1:] + face[:1], strict=True))
    assert len(directed) == len(set(directed))
    # signed volume of the closed mesh = floor area x height (outward normals)
    vol, k = 0.0, 0
    for c in counts:
        f = [pts[i] for i in idx[k : k + c]]
        k += c
        for i in range(1, c - 1):
            vol += np.dot(f[0], np.cross(f[i], f[i + 1])) / 6
    assert abs(abs(vol) - L_AREA * 2.6) / (L_AREA * 2.6) < 0.04
    info = write_usda(sm, tmp_path / "r.usda")
    text = (tmp_path / "r.usda").read_text()
    assert text.startswith("#usda 1.0") and 'upAxis = "Z"' in text and info["faces"] == len(counts)


@pytest.mark.light
def test_laz_roundtrip_within_1mm(maps, tmp_path):
    sm = maps["box"]
    n = write_laz(sm.fused_cloud, tmp_path / "c.laz")
    import open3d as o3d

    src = np.asarray(o3d.io.read_point_cloud(str(sm.fused_cloud)).points)
    back = read_laz_xyz(tmp_path / "c.laz")
    assert n == len(src) == len(back)
    assert np.abs(back - src).max() <= 0.0006  # 1 mm quantisation
    assert (tmp_path / "c.laz").stat().st_size < src.nbytes  # actually compressed


@pytest.mark.light
def test_cli_all_writes_every_file(maps, tmp_path):
    from typer.testing import CliRunner

    from export.cli import app

    r = CliRunner().invoke(
        app, ["all", "--spatial-map", str(maps["box"].root), "--out", str(tmp_path / "o")]
    )
    assert r.exit_code == 0, r.output
    out = json.loads(r.output.strip().splitlines()[-1])
    assert out["skipped"] == {}
    for f in (
        "floorplan.svg",
        "floorplan.dxf",
        "room_shell.usda",
        "fused_cloud.laz",
        "measurements.json",
    ):
        assert (tmp_path / "o" / f).stat().st_size > 100
