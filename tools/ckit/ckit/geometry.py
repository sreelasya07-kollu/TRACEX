"""Geometry conventions for Python callers: thin wrappers over the C++ core (one implementation).

Poses are (t[3], q[4]) with q = [w, x, y, z]. pycolmap's Rotation3d.quat is [x, y, z, w]: always go
through `wxyz_to_xyzw` / `xyzw_to_wxyz` when crossing that boundary.
"""

from __future__ import annotations

from ckit.core import load


def _g():
    return load().geometry


def from_arkit(t, q_wxyz):
    return _g().from_arkit(list(t), list(q_wxyz))


def from_arcore(t, q_xyzw):
    return _g().from_arcore(list(t), list(q_xyzw))


def to_colmap(t, q_wxyz, intrinsics: dict) -> dict:
    return _g().to_colmap(list(t), list(q_wxyz), intrinsics)


def to_opengl(t, q_wxyz, intrinsics: dict, z_near: float = 0.05, z_far: float = 100.0) -> dict:
    return _g().to_opengl(list(t), list(q_wxyz), intrinsics, z_near, z_far)


def rotate_image(t, q_wxyz, intrinsics: dict, deg_cw: int):
    return _g().rotate_image(list(t), list(q_wxyz), intrinsics, deg_cw)


def scale_intrinsics(intrinsics: dict, width: int, height: int) -> dict:
    return _g().scale_intrinsics(intrinsics, width, height)


def crop_intrinsics(intrinsics: dict, x0: int, y0: int, width: int, height: int) -> dict:
    return _g().crop_intrinsics(intrinsics, x0, y0, width, height)


def project(intrinsics: dict, t, q_wxyz, points_world) -> list:
    pts = [list(map(float, p)) for p in points_world]
    return _g().project(intrinsics, list(t), list(q_wxyz), pts)


def interpolate_at(t_ns, t, q_wxyz, query_ns, max_gap_ns: int) -> list:
    return _g().interpolate_at(list(t_ns), list(t), list(q_wxyz), list(query_ns), max_gap_ns)


def nearest_lookup(t_ns, query_ns, tolerance_ns: int) -> list:
    return _g().nearest_lookup(list(t_ns), list(query_ns), tolerance_ns)


def wxyz_to_xyzw(q):
    w, x, y, z = q
    return [x, y, z, w]


def xyzw_to_wxyz(q):
    x, y, z, w = q
    return [w, x, y, z]
