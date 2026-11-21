"""Phase 5: planes + room shell from a fused, gravity-aligned cloud (world +Z up, metres).

1. voxel downsample; 2. iterative RANSAC (Open3D `segment_plane`) with a normal-agreement
filter so a plane cannot collect points from crossing surfaces; 3. least-squares refit;
4. merge near-coplanar planes; 5. label by gravity: floor = lowest large horizontal plane,
ceiling = highest, walls = large vertical planes spanning a good part of the room height
and (when a ceiling exists) reaching up to it, which rejects cabinet and table faces;
6. floor polygon from consecutive wall-line intersections, or the wall-snapped footprint
contour when it shows a recess / bay (fallback: convex hull of floor inliers); ceiling
height = ceiling_z - floor_z. Deterministic for a fixed `seed`.
"""

from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np
import open3d as o3d
from scipy.spatial import ConvexHull

UP = np.array([0.0, 0.0, 1.0])


@dataclass
class PlaneParams:
    voxel: float = 0.03
    dist_thresh: float = 0.03  # RANSAC inlier distance [m]
    normal_thresh_deg: float = 25.0  # inlier normal must agree with plane normal
    ransac_iters: int = 2000
    min_inliers: int = 300
    max_planes: int = 40
    max_failures: int = 10
    merge_angle_deg: float = 5.0
    merge_offset_m: float = 0.05
    horiz_tol_deg: float = 10.0  # |angle(n, Z)| for horizontal / vertical tests
    min_floor_area_m2: float = 1.0
    min_wall_area_m2: float = 0.8
    min_wall_height_frac: float = 0.5  # of floor->ceiling distance (or 1.0 m if unknown)
    wall_cluster_deg: float = 20.0  # walls this parallel (same facing) form one direction
    footprint_cell_m: float = 0.03  # top-down occupancy grid for the floor outline
    footprint_close_m: float = 0.30  # closes gaps in the wall ring (unseen / occluded wall)
    footprint_band_m: float = 0.30  # ignore points this close to floor / ceiling (TSDF smear)
    footprint_simplify_m: float = 0.05  # Douglas-Peucker tolerance
    # contour replaces the wall polygon if they differ by a blob this wide
    footprint_blob_m: float = 0.20
    snap_dist_m: float = 0.12  # contour edge -> wall line: max midpoint distance
    snap_angle_deg: float = 10.0  # contour edge -> wall line: max direction difference
    wall_top_tol_m: float = 0.35  # with a ceiling: wall inliers must reach ceiling_z - tol
    seed: int = 0


@dataclass
class DetectedPlane:
    normal: np.ndarray  # unit, oriented toward the cloud centroid (into the room)
    d: float  # n.x + d = 0
    idx: np.ndarray  # inlier indices into the downsampled cloud
    label: str = "other"

    def fit(self, P: np.ndarray, centroid: np.ndarray):
        X = P[self.idx]
        c = X.mean(0)
        n = np.linalg.svd(X - c, full_matrices=False)[2][-1]
        if n @ (centroid - c) < 0:
            n = -n
        self.normal, self.d = n, float(-n @ c)

    def basis(self):
        a = np.cross(self.normal, UP)
        if np.linalg.norm(a) < 1e-6:  # horizontal plane: use world x/y
            a = np.array([1.0, 0, 0])
        a /= np.linalg.norm(a)
        return a, np.cross(self.normal, a)

    def hull(self, P: np.ndarray):
        """(polygon 3D points on the plane, area m²) of the projected inlier convex hull."""
        X = P[self.idx]
        X = X - np.outer(X @ self.normal + self.d, self.normal)
        a, b = self.basis()
        uv = np.c_[X @ a, X @ b]
        try:
            h = ConvexHull(uv)
        except Exception:  # noqa: BLE001 - degenerate (collinear) inliers
            return [], 0.0
        o = -self.d * self.normal
        poly = [o + (uv[k, 0] - o @ a) * a + (uv[k, 1] - o @ b) * b for k in h.vertices]
        return [p.tolist() for p in poly], float(h.volume)

    def rms(self, P: np.ndarray) -> float:
        return float(np.sqrt(np.mean((P[self.idx] @ self.normal + self.d) ** 2)))


def _normals(pcd: o3d.geometry.PointCloud, voxel: float) -> np.ndarray:
    if not pcd.has_normals():
        pcd.estimate_normals(o3d.geometry.KDTreeSearchParamHybrid(3 * voxel, 30))
    return np.asarray(pcd.normals)


def detect_planes(pcd: o3d.geometry.PointCloud, p: PlaneParams):
    """Returns (downsampled points (N,3), list[DetectedPlane])."""
    o3d.utility.random.seed(p.seed)
    ds = pcd.voxel_down_sample(p.voxel)
    if ds.has_normals():  # averaged normals: renormalise; re-estimate where degenerate
        ds.normalize_normals()
    P = np.asarray(ds.points)
    N = _normals(ds, p.voxel)
    centroid = np.median(P, 0)
    cos_n = np.cos(np.radians(p.normal_thresh_deg))
    remaining = np.arange(len(P))
    planes: list[DetectedPlane] = []
    failures = 0
    while len(remaining) >= p.min_inliers and len(planes) < p.max_planes:
        sub = ds.select_by_index(remaining.tolist())
        model, inl = sub.segment_plane(p.dist_thresh, 3, p.ransac_iters)
        inl = remaining[np.asarray(inl, int)]
        n = np.asarray(model[:3]) / np.linalg.norm(model[:3])
        d = model[3] / np.linalg.norm(model[:3])
        keep = inl[np.abs(N[inl] @ n) >= cos_n]
        if len(keep) >= p.min_inliers:
            pl = DetectedPlane(n, d, keep)
            pl.fit(P, centroid)
            # re-gather on the refit plane, among remaining points
            r = remaining
            ok = (np.abs(P[r] @ pl.normal + pl.d) < p.dist_thresh) & (
                np.abs(N[r] @ pl.normal) >= cos_n
            )
            pl.idx = r[ok]
            pl.fit(P, centroid)
            planes.append(pl)
            remaining = np.setdiff1d(remaining, pl.idx, assume_unique=True)
            failures = 0
        else:
            # RANSAC found a cross-cutting plane: drop its points from the search pool only
            remaining = np.setdiff1d(remaining, inl, assume_unique=True)
            failures += 1
            if failures >= p.max_failures:
                break
    return P, merge_planes(P, planes, centroid, p)


def merge_planes(P, planes, centroid, p: PlaneParams) -> list[DetectedPlane]:
    cos_m = np.cos(np.radians(p.merge_angle_deg))
    merged = True
    while merged:
        merged = False
        for i in range(len(planes)):
            for j in range(i + 1, len(planes)):
                a, b = planes[i], planes[j]
                if a.normal @ b.normal < cos_m:
                    continue
                # offset difference measured at each plane's inlier centroid
                ca, cb = P[a.idx].mean(0), P[b.idx].mean(0)
                if max(abs(cb @ a.normal + a.d), abs(ca @ b.normal + b.d)) > p.merge_offset_m:
                    continue
                a.idx = np.union1d(a.idx, b.idx)
                a.fit(P, centroid)
                planes.pop(j)
                merged = True
                break
            if merged:
                break
    # deterministic order: by inlier count, descending
    return sorted(planes, key=lambda q: -len(q.idx))


def label_planes(P, planes: list[DetectedPlane], p: PlaneParams) -> dict:
    sin_t = np.sin(np.radians(p.horiz_tol_deg))
    cos_t = np.cos(np.radians(p.horiz_tol_deg))
    areas = [pl.hull(P)[1] for pl in planes]
    horiz = [
        k
        for k, pl in enumerate(planes)
        if abs(pl.normal[2]) >= cos_t and areas[k] >= p.min_floor_area_m2
    ]
    # height at the inliers (not at x=y=0, which is wrong for slightly tilted planes)
    z = {k: float(np.median(P[planes[k].idx, 2])) for k in horiz}
    floor = min(horiz, key=lambda k: z[k]) if horiz else None
    ceil_c = [k for k in horiz if k != floor and planes[k].normal[2] < 0]  # faces down
    ceiling = max(ceil_c, key=lambda k: z[k]) if ceil_c else None
    if floor is not None and planes[floor].normal[2] < 0:
        floor = None  # lowest horizontal faces down: not a floor seen from inside
    floor_z = z[floor] if floor is not None else None
    ceil_z = z[ceiling] if ceiling is not None else None
    room_h = None
    if floor is not None and ceiling is not None:
        # perpendicular floor->ceiling distance at the floor inlier centroid: robust to a
        # residual tilt of the world frame (z differences are not)
        cf = P[planes[floor].idx].mean(0)
        room_h = float(abs(planes[ceiling].normal @ cf + planes[ceiling].d))
    min_h = p.min_wall_height_frac * room_h if room_h else 1.0
    walls = []
    for k, pl in enumerate(planes):
        if k in (floor, ceiling):
            pl.label = "floor" if k == floor else "ceiling"
        elif abs(pl.normal[2]) <= sin_t and areas[k] >= p.min_wall_area_m2:
            zs = P[pl.idx, 2]
            span = np.percentile(zs, 98) - np.percentile(zs, 2)
            reaches_top = ceil_z is None or np.percentile(zs, 98) >= ceil_z - p.wall_top_tol_m
            pl.label = "wall" if span >= min_h and reaches_top else "other"
        else:
            pl.label = "other"
        if pl.label == "wall":
            walls.append(k)
    return {
        "floor": floor,
        "ceiling": ceiling,
        "walls": walls,
        "floor_z": floor_z,
        "ceiling_z": ceil_z,
        "ceiling_height": room_h,
        "areas": areas,
    }


def _shoelace(poly: np.ndarray) -> float:
    x, y = poly[:, 0], poly[:, 1]
    return 0.5 * float(np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1)))


def _segments_cross(a, b, c, d) -> bool:
    def o(p, q, r):
        return np.sign((q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]))

    return o(a, b, c) * o(a, b, d) < 0 and o(c, d, a) * o(c, d, b) < 0


def _is_simple(poly: np.ndarray) -> bool:
    n = len(poly)
    for i in range(n):
        for j in range(i + 2, n):
            if i == 0 and j == n - 1:
                continue
            if _segments_cross(poly[i], poly[(i + 1) % n], poly[j], poly[(j + 1) % n]):
                return False
    return True


def dominant_walls(P, planes, walls: list[int], p: PlaneParams) -> list[int]:
    """One wall per direction cluster (normals within `wall_cluster_deg`): the largest by
    inlier count. Recesses/bays (parallel walls at other offsets) are handled by the footprint
    contour in `floor_polygon`, not here."""
    cos_c = np.cos(np.radians(p.wall_cluster_deg))
    keep: list[int] = []
    for k in sorted(walls, key=lambda k: -len(planes[k].idx)):
        if all(planes[k].normal @ planes[j].normal < cos_c for j in keep):
            keep.append(k)
    return sorted(keep)


def _raster_poly(poly: np.ndarray, x0: float, y0: float, cell: float, shape) -> np.ndarray:
    m = np.zeros(shape, np.uint8)
    px = np.round((poly - [x0, y0]) / cell - 0.5).astype(np.int32)
    cv2.fillPoly(m, [px.reshape(-1, 1, 2)], 1)
    return m.astype(bool)


def footprint_polygon(P, lab, p: PlaneParams):
    """Floor outline from the top-down occupancy of *vertical* structure (points between
    `footprint_band_m` above the floor and below the ceiling: walls, furniture). Floor and
    ceiling surfaces are excluded because TSDF fusion smears them a few cm past the walls.
    The wall ring is closed morphologically, the exterior flood-filled, and the interior
    contoured: this captures recesses and bays that one-line-per-direction walls cannot.
    Returns (polygon (N,2) CCW | None, (x0, y0, cell, shape)). None when the ring does not
    close (the interior then leaks), so callers fall back to the wall-intersection polygon."""
    if lab["floor_z"] is None:
        return None, None
    lo = lab["floor_z"] + p.footprint_band_m
    hi = (lab["ceiling_z"] - p.footprint_band_m) if lab["ceiling_z"] is not None else np.inf
    Q = P[(P[:, 2] >= lo) & (P[:, 2] <= hi), :2]
    if len(Q) < 200:
        return None, None
    cell = p.footprint_cell_m
    x0, y0 = Q.min(0) - 6 * cell
    ij = np.floor((Q - [x0, y0]) / cell).astype(int)
    shape = (ij[:, 1].max() + 7, ij[:, 0].max() + 7)
    cnt = np.zeros(shape, np.int32)
    np.add.at(cnt, (ij[:, 1], ij[:, 0]), 1)
    occ = (cnt >= 2).astype(np.uint8)
    k = int(round(p.footprint_close_m / cell)) | 1
    occ = cv2.morphologyEx(occ, cv2.MORPH_CLOSE, cv2.getStructuringElement(cv2.MORPH_RECT, (k, k)))
    ff = occ.copy()
    mask = np.zeros((shape[0] + 2, shape[1] + 2), np.uint8)
    cv2.floodFill(ff, mask, (0, 0), 2)  # exterior = 2
    interior = (ff != 2).astype(np.uint8)  # walls + enclosed floor
    n, lbl, st, _ = cv2.connectedComponentsWithStats(interior, connectivity=8)
    if n < 2:
        return None, None
    interior = (lbl == 1 + int(np.argmax(st[1:, cv2.CC_STAT_AREA]))).astype(np.uint8)
    cs, _ = cv2.findContours(interior, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    c = max(cs, key=cv2.contourArea)
    c = cv2.approxPolyDP(c, p.footprint_simplify_m / cell, True).reshape(-1, 2).astype(float)
    poly = np.c_[x0 + (c[:, 0] + 0.5) * cell, y0 + (c[:, 1] + 0.5) * cell]
    if len(poly) < 3:
        return None, None
    if _shoelace(poly) < 0:
        poly = poly[::-1]
    return poly, (x0, y0, cell, shape)


def _wall_lines(planes, walls: list[int]) -> list[tuple[np.ndarray, float]]:
    """(unit 2D normal, offset) of each labelled wall, in `walls` order."""
    out = []
    for k in walls:
        n2 = planes[k].normal[:2]
        s = np.linalg.norm(n2)
        out.append((n2 / s, planes[k].d / s))
    return out


def _snap_contour(foot: np.ndarray, lines, p: PlaneParams, min_corner_angle_deg: float = 30.0):
    """Snap the grid-quantised contour edges (>= 0.20 m) to detected wall lines (direction within
    `snap_angle_deg`, midpoint within `snap_dist_m`, nearest wins), merge consecutive edges on the
    same wall and rebuild vertices by line intersection. A vertex whose two lines are nearly
    parallel keeps its raw position; next to an unmatched edge it is projected on the snapped
    line. Returns the snapped polygon, or `foot` unchanged when nothing matches or the result is
    not simple / not within 3 % of the raw area."""
    n = len(foot)
    if n < 3 or not lines:
        return foot
    sin_a = np.sin(np.radians(p.snap_angle_deg))
    sin_min = np.sin(np.radians(min_corner_angle_deg))
    asg: list[int | None] = []
    for i in range(n):
        a, b = foot[i], foot[(i + 1) % n]
        e = b - a
        length = float(np.linalg.norm(e))
        best, best_d = None, np.inf
        if length >= 0.20:
            u, m = e / length, 0.5 * (a + b)
            for w, (n2, d2) in enumerate(lines):
                dist = abs(float(n2 @ m) + d2)
                if abs(float(n2 @ u)) <= sin_a and dist <= p.snap_dist_m and dist < best_d:
                    best, best_d = w, dist
        asg.append(best)
    if all(a is None for a in asg):
        return foot

    starts = [i for i in range(n) if not (asg[i] is not None and asg[i] == asg[i - 1])]
    if not starts:
        return foot  # one wall for the whole outline: degenerate
    groups = [(asg[g], g) for g in starts]  # (wall | None, first edge index)
    verts = []
    for gi, (w, first) in enumerate(groups):
        wp = groups[gi - 1][0]
        raw = foot[first]
        if w is not None and wp is not None:
            (n1, d1), (n2, d2) = lines[wp], lines[w]
            A = np.array([n1, n2])
            if abs(np.linalg.det(A)) >= sin_min:
                verts.append(np.linalg.solve(A, -np.array([d1, d2])))
            else:
                verts.append(raw)
        elif w is not None or wp is not None:
            n1, d1 = lines[w if w is not None else wp]
            verts.append(raw - (float(n1 @ raw) + d1) * n1)
        else:
            verts.append(raw)
    poly = np.array(verts)
    if len(poly) < 3:
        return foot
    if _shoelace(poly) < 0:
        poly = poly[::-1]
    area, raw_area = _shoelace(poly), _shoelace(foot)
    if not (_is_simple(poly) and area > 0 and abs(area - raw_area) <= 0.03 * raw_area):
        return foot
    return poly


def _has_blob(a: np.ndarray, b: np.ndarray, p: PlaneParams) -> bool:
    """True when the symmetric difference of two rasters holds a blob wider than
    `footprint_blob_m` (a recess or bay); thin TSDF edge slivers are removed by the opening."""
    k = int(round(p.footprint_blob_m / p.footprint_cell_m)) | 1
    diff = (a ^ b).astype(np.uint8)
    opened = cv2.morphologyEx(
        diff, cv2.MORPH_OPEN, cv2.getStructuringElement(cv2.MORPH_RECT, (k, k))
    )
    return bool(opened.any())


def floor_polygon(P, planes, lab, p: PlaneParams, min_corner_angle_deg: float = 30.0):
    """2D CCW floor polygon. Candidates, in order: (1) intersect consecutive dominant-wall lines
    (ordered by the angle of their inlier centroid around the room centre; near-parallel
    neighbours skipped), validated (simple, >= 3 corners, area within [0.7, 1.6]x the floor-hull
    area when a floor exists), kept when the footprint contour adds no blob (recess / bay wider
    than `footprint_blob_m`) relative to it; (2) the footprint contour snapped to the wall
    lines, used when the wall polygon is invalid or differs by such a blob (the contour is
    computed whenever >= 3 walls exist or the wall polygon is invalid); (3) fallback: convex
    hull of floor inliers."""
    hull_poly, hull_area = None, None
    if lab["floor"] is not None:
        xy = P[planes[lab["floor"]].idx, :2]
        h = ConvexHull(xy)
        hull_poly, hull_area = xy[h.vertices], float(h.volume)
    walls = dominant_walls(P, planes, lab["walls"], p)
    poly, area = None, None  # validated wall-intersection polygon
    if len(walls) >= 3:
        cs = np.array([P[planes[k].idx, :2].mean(0) for k in walls])
        centre = cs.mean(0) if hull_poly is None else hull_poly.mean(0)
        order = np.argsort(np.arctan2(cs[:, 1] - centre[1], cs[:, 0] - centre[0]))
        lines = _wall_lines(planes, [walls[o] for o in order])
        sin_min = np.sin(np.radians(min_corner_angle_deg))
        corners = []
        for i in range(len(lines)):
            (n1, d1), (n2, d2) = lines[i], lines[(i + 1) % len(lines)]
            A = np.array([n1, n2])
            if abs(np.linalg.det(A)) < sin_min:
                continue
            corners.append(np.linalg.solve(A, -np.array([d1, d2])))
        if len(corners) >= 3:
            cand = np.array(corners)
            if _shoelace(cand) < 0:
                cand = cand[::-1]
            a_ = _shoelace(cand)
            ok = _is_simple(cand) and a_ > 0
            if ok and hull_area is not None:
                ok = 0.7 * hull_area <= a_ <= 1.6 * hull_area
            if ok:
                poly, area = cand, a_
    if len(lab["walls"]) >= 3 or poly is None:
        foot, grid = footprint_polygon(P, lab, p)
        if foot is not None and hull_area is not None:
            if not (0.5 * hull_area <= _shoelace(foot) <= 1.3 * hull_area):
                foot = None  # ring leaked or footprint implausible
        if foot is not None:
            use_foot = poly is None  # no valid wall polygon: the contour is the better candidate
            if poly is not None:
                x0, y0, cell, shape = grid
                try:
                    a = _raster_poly(poly, x0, y0, cell, shape)
                except cv2.error:
                    a = np.zeros(shape, bool)
                use_foot = _has_blob(a, _raster_poly(foot, x0, y0, cell, shape), p)
            if use_foot:
                snapped = _snap_contour(foot, _wall_lines(planes, lab["walls"]), p)
                return snapped, _shoelace(snapped), "footprint_contour"
    if poly is not None:
        return poly, area, "wall_intersections"
    if hull_poly is not None:
        poly = hull_poly if _shoelace(hull_poly) > 0 else hull_poly[::-1]
        return poly, _shoelace(poly), "floor_hull"
    return np.zeros((0, 2)), None, "none"
