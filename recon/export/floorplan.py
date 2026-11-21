"""Floor plan exports from room.json: SVG, DXF (ezdxf) and a measurement summary."""

from __future__ import annotations

from pathlib import Path

import ezdxf
import numpy as np

from export.geometry import area, edge_lengths
from formats.spatial_map import SpatialMap


def _poly(sm: SpatialMap) -> np.ndarray:
    p = np.asarray(sm.room.floor_polygon, float)
    if len(p) < 3:
        raise ValueError("room has no floor polygon (see quality.json warnings)")
    return p


def write_svg(sm: SpatialMap, path: Path, px_per_m: float = 100.0, margin_m: float = 0.6) -> None:
    """Floor outline with one dimension label per edge. y is flipped (plan y up, SVG y down)."""
    p = _poly(sm)
    lo, hi = p.min(0) - margin_m, p.max(0) + margin_m
    w, h = (hi - lo) * px_per_m

    def pt(q):
        return (q[0] - lo[0]) * px_per_m, (hi[1] - q[1]) * px_per_m

    pts = " ".join(f"{x:.1f},{y:.1f}" for x, y in map(pt, p))
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w:.0f}" height="{h:.0f}" '
        f'viewBox="0 0 {w:.1f} {h:.1f}">',
        f'<polygon points="{pts}" fill="#eef3f8" stroke="#1d3557" stroke-width="3"/>',
    ]
    for a, b, length in zip(p, np.roll(p, -1, 0), edge_lengths(p), strict=True):
        (x, y) = pt((a + b) / 2)
        out.append(
            f'<text x="{x:.1f}" y="{y:.1f}" font-size="14" text-anchor="middle" '
            f'fill="#1d3557">{length:.2f} m</text>'
        )
    c = pt(p.mean(0))
    r = sm.room
    label = f"{r.floor_area_m2:.1f} m²" + (
        f", ceiling {r.ceiling_height_m:.2f} m" if r.ceiling_height_m else ""
    )
    out.append(
        f'<text x="{c[0]:.1f}" y="{c[1]:.1f}" font-size="18" text-anchor="middle">{label}</text>'
    )
    out.append("</svg>")
    path.write_text("\n".join(out))


def write_dxf(sm: SpatialMap, path: Path) -> None:
    """DXF R2010, metres ($INSUNITS = 6). Layers: FLOOR_OUTLINE (closed polyline), WALLS (one
    line per detected wall plane, from the plane's footprint), DIMENSIONS (edge lengths), NOTES."""
    p = _poly(sm)
    doc = ezdxf.new("R2010")
    doc.header["$INSUNITS"] = 6
    for name, color in (("FLOOR_OUTLINE", 5), ("WALLS", 1), ("DIMENSIONS", 3), ("NOTES", 7)):
        doc.layers.add(name, color=color)
    msp = doc.modelspace()
    msp.add_lwpolyline([tuple(q) for q in p], close=True, dxfattribs={"layer": "FLOOR_OUTLINE"})
    for a, b, length in zip(p, np.roll(p, -1, 0), edge_lengths(p), strict=True):
        m = (a + b) / 2
        msp.add_text(
            f"{length:.2f}", height=0.12, dxfattribs={"layer": "DIMENSIONS", "insert": tuple(m)}
        )
    for pl in sm.planes.planes:
        if pl.label != "wall" or len(pl.polygon) < 2:
            continue
        xy = np.asarray(pl.polygon)[:, :2]
        d = np.asarray(pl.normal[:2])[::-1] * [1, -1]  # wall direction (perpendicular to normal)
        t = xy @ d
        msp.add_line(tuple(xy[t.argmin()]), tuple(xy[t.argmax()]), dxfattribs={"layer": "WALLS"})
    r = sm.room
    note = f"area {r.floor_area_m2:.2f} m2"
    if r.ceiling_height_m:
        note += f", ceiling {r.ceiling_height_m:.2f} m"
    note += f", scale confidence {r.scale_confidence}"
    msp.add_text(note, height=0.15, dxfattribs={"layer": "NOTES", "insert": tuple(p.min(0))})
    doc.saveas(str(path))


def read_dxf_outline(path: Path) -> np.ndarray:
    """Floor outline vertices read back from a DXF written by `write_dxf` (round-trip checks)."""
    doc = ezdxf.readfile(str(path))
    pl = next(e for e in doc.modelspace().query("LWPOLYLINE") if e.dxf.layer == "FLOOR_OUTLINE")
    return np.array([(x, y) for x, y, *_ in pl.get_points()])


def measurements(sm: SpatialMap) -> dict:
    p = _poly(sm)
    r = sm.room
    h = r.ceiling_height_m
    a = area(p)
    return {
        "floor_area_m2": a,
        "perimeter_m": float(sum(edge_lengths(p))),
        "wall_lengths_m": edge_lengths(p),
        "ceiling_height_m": h,
        "volume_m3": a * h if h else None,
        "bbox_m": [float(v) for v in np.ptp(p, axis=0)],
        "floor_polygon_source": r.floor_polygon_source,
        "scale_confidence": r.scale_confidence,
        "scale_source": r.scale_source,
        "warnings": sm.quality.warnings,
    }
