"""Per-surface observation coverage from the camera poses (no occlusion model: furniture can
hide a wall that this model counts as seen; documented limitation).

Surfaces come from the ROOM MODEL (floor polygon extruded to the ceiling), not from measured
points, so a wall nobody looked at is found even though it has no points. Each surface is
sampled on a grid; a cell counts as observed when >= min_views cameras see it (in front, inside
the image, not at a grazing angle) with a viewing-ray spread >= min_view_spread_deg."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import ndimage

from formats.models import RescanRegion
from formats.spatial_map import SpatialMap
from qa.thresholds import Thresholds


@dataclass
class Surface:
    kind: str  # wall | floor | ceiling
    wall_index: int | None
    origin: np.ndarray  # 3D point of the grid origin
    u: np.ndarray  # unit axes of the surface (3D)
    v: np.ndarray
    normal: np.ndarray  # pointing into the room
    nu: int
    nv: int
    cell: float
    mask: np.ndarray  # (nv, nu) cells that belong to the surface (inside polygon for floor)

    def centres(self) -> np.ndarray:
        gu, gv = np.meshgrid(np.arange(self.nu) + 0.5, np.arange(self.nv) + 0.5)
        return self.origin + (gu[..., None] * self.u + gv[..., None] * self.v) * self.cell

    def to_world(self, iu: float, iv: float) -> np.ndarray:
        return self.origin + (iu * self.u + iv * self.v) * self.cell


def _inside_polygon(pts: np.ndarray, poly: np.ndarray) -> np.ndarray:
    import cv2

    scale = 1000.0
    m = np.zeros(len(pts), bool)
    cnt = (poly * scale).astype(np.int32).reshape(-1, 1, 2)
    for k, p in enumerate(pts):
        m[k] = cv2.pointPolygonTest(cnt, (float(p[0] * scale), float(p[1] * scale)), False) >= 0
    return m


def build_surfaces(sm: SpatialMap, cell: float) -> list[Surface]:
    r = sm.room
    poly = np.asarray(r.floor_polygon, float)
    if len(poly) < 3 or r.floor_z is None:
        return []
    z0 = r.floor_z
    z1 = r.ceiling_z if r.ceiling_z is not None else z0 + 2.5
    out: list[Surface] = []
    z = np.array([0.0, 0.0, 1.0])
    n = len(poly)
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        length = float(np.linalg.norm(b - a))
        if length < 2 * cell:
            continue
        d = np.array([*(b - a) / length, 0.0])
        normal = np.array([-d[1], d[0], 0.0])  # left of a CCW edge = into the room
        nu, nv = int(np.ceil(length / cell)), int(np.ceil((z1 - z0) / cell))
        out.append(
            Surface(
                "wall", i, np.array([*a, z0]), d, z, normal, nu, nv, cell, np.ones((nv, nu), bool)
            )
        )
    lo, hi = poly.min(0), poly.max(0)
    nu, nv = int(np.ceil((hi[0] - lo[0]) / cell)), int(np.ceil((hi[1] - lo[1]) / cell))
    for kind, zz, nrm in (("floor", z0, z), ("ceiling", z1, -z)):
        s = Surface(
            kind,
            None,
            np.array([lo[0], lo[1], zz]),
            np.array([1.0, 0, 0]),
            np.array([0, 1.0, 0]),
            nrm,
            nu,
            nv,
            cell,
            np.zeros((nv, nu), bool),
        )
        c = s.centres().reshape(-1, 3)
        s.mask = _inside_polygon(c[:, :2], poly).reshape(nv, nu)
        out.append(s)
    return out


def view_counts(sm: SpatialMap, s: Surface, th: Thresholds) -> tuple[np.ndarray, np.ndarray]:
    """(number of views, max viewing-ray spread in degrees) per cell."""
    K = sm.cameras.intrinsics.K()
    W, H = sm.cameras.width, sm.cameras.height
    P = s.centres().reshape(-1, 3)
    count = np.zeros(len(P), int)
    rays: list[np.ndarray] = []
    seen: list[np.ndarray] = []
    for T in sm.cameras.frames.values():
        T = np.asarray(T)
        c = T[:3, 3]
        pc = (P - c) @ T[:3, :3]  # camera-frame coordinates
        z = pc[:, 2]
        with np.errstate(divide="ignore", invalid="ignore"):
            u = K[0, 0] * pc[:, 0] / z + K[0, 2]
            v = K[1, 1] * pc[:, 1] / z + K[1, 2]
        to_cam = c - P
        dist = np.linalg.norm(to_cam, axis=1)
        facing = (to_cam @ s.normal) / np.maximum(dist, 1e-9)
        ok = (z > 0.1) & (u >= 0) & (u < W) & (v >= 0) & (v < H) & (facing > th.max_grazing_cos)
        count += ok
        seen.append(ok)
        rays.append(to_cam / np.maximum(dist, 1e-9)[:, None])
    spread = np.zeros(len(P))
    if rays:
        R = np.stack(rays)  # (cams, cells, 3)
        S = np.stack(seen)
        for k in np.flatnonzero(count >= 2):  # max pairwise angle among viewing cameras
            r = R[S[:, k], k]
            cosm = np.clip(r @ r.T, -1, 1).min()
            spread[k] = np.degrees(np.arccos(cosm))
    return count.reshape(s.nv, s.nu), spread.reshape(s.nv, s.nu)


def _axis_name(n: np.ndarray) -> str:
    k = int(np.argmax(np.abs(n[:2])))
    return ("+" if n[k] > 0 else "-") + "XY"[k]


def _instruction(s: Surface, bbox: tuple[int, int, int, int], reason: str) -> str:
    u0, u1, v0, v1 = bbox
    c = s.cell
    if s.kind == "wall":
        side = f"facing {_axis_name(s.normal)}"
        return (
            f"Wall {s.wall_index} ({side}, world axes): {reason}; rescan between "
            f"{u0 * c:.1f} m and {u1 * c:.1f} m along the wall (from its start corner at "
            f"x={s.origin[0]:.1f}, y={s.origin[1]:.1f}), "
            f"{v0 * c:.1f} m to {v1 * c:.1f} m above the floor."
        )
    a, b = s.to_world(u0, v0), s.to_world(u1, v1)
    return (
        f"{s.kind.capitalize()}: {reason}; rescan the area x {min(a[0], b[0]):.1f} to "
        f"{max(a[0], b[0]):.1f} m, y {min(a[1], b[1]):.1f} to {max(a[1], b[1]):.1f} m "
        f"(world axes)."
    )


def analyse(sm: SpatialMap, th: Thresholds | None = None):
    """Returns (coverage dict, list[RescanRegion])."""
    th = th or Thresholds()
    surfaces = build_surfaces(sm, th.cell_m)
    per_kind: dict[str, list[float]] = {"wall": [0, 0], "floor": [0, 0], "ceiling": [0, 0]}
    regions: list[RescanRegion] = []
    total_area = sum(s.mask.sum() for s in surfaces) * th.cell_m**2 or 1.0
    for s in surfaces:
        cnt, spread = view_counts(sm, s, th)
        good = (cnt >= th.min_views) & (spread >= th.min_view_spread_deg)
        per_kind[s.kind][0] += int((good & s.mask).sum())
        per_kind[s.kind][1] += int(s.mask.sum())
        bad = s.mask & ~good
        lbl, n = ndimage.label(bad)
        for k in range(1, n + 1):
            cells = lbl == k
            area = float(cells.sum() * th.cell_m**2)
            if area < th.min_region_area_m2:
                continue
            vv, uu = np.nonzero(cells)
            u0, u1, v0, v1 = uu.min(), uu.max() + 1, vv.min(), vv.max() + 1
            seen_any = cnt[cells].max() > 0
            reason = (
                "unseen"
                if not seen_any
                else ("few_views" if cnt[cells].max() < th.min_views else "narrow_baseline")
            )
            text = {
                "unseen": "never seen",
                "few_views": "seen from too few views",
                "narrow_baseline": "seen only from nearly the same position",
            }[reason]
            corners = [
                s.to_world(u0, v0),
                s.to_world(u1, v0),
                s.to_world(u1, v1),
                s.to_world(u0, v1),
            ]
            weight = 1.0 if s.kind == "wall" else (0.8 if s.kind == "floor" else 0.4)
            regions.append(
                RescanRegion(
                    surface=s.kind,
                    wall_index=s.wall_index,
                    reason=reason,
                    corners=[[float(x) for x in c] for c in corners],
                    area_m2=area,
                    priority=float(min(1.0, weight * area / total_area * 4)),
                    instruction=_instruction(s, (u0, u1, v0, v1), text),
                )
            )
    cov = {k: (v[0] / v[1] if v[1] else 0.0) for k, v in per_kind.items()}
    regions.sort(key=lambda r: -r.priority)
    return cov, regions
