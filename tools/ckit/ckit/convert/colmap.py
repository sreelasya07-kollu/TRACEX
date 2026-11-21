"""`ckit export-colmap`: Scan Bundle -> COLMAP text model with known poses.

cameras.txt (one camera per distinct intrinsics), images.txt (cam_from_world, QW QX QY QZ, empty
2D line), empty points3D.txt. Poses are interpolated at each keyframe's t_ns (no extrapolation);
keyframes without a NORMAL-tracking bracket are skipped and counted.
"""

from __future__ import annotations

import bisect
import json
from pathlib import Path

from ckit import geometry
from ckit.bundle.streams import read_streams

MODELS = {0: "PINHOLE", 1: "OPENCV"}
NORMAL = 1


def _intrinsics_dict(m) -> dict:
    return {
        "width": m.width,
        "height": m.height,
        "fx": m.fx,
        "fy": m.fy,
        "cx": m.cx,
        "cy": m.cy,
        "model": MODELS.get(int(m.model), "PINHOLE"),
        "distortion": list(m.distortion),
    }


def load_keyframe_poses(bundle: Path, max_gap_ns: int = 100_000_000) -> dict:
    """Keyframes with interpolated T_world_from_camera and their intrinsics."""
    s = read_streams(bundle / "streams.mcap")
    poses = [p for p in s.get("/pose") if p.tracking_state == NORMAL]
    intr = s.get("/intrinsics")
    if not intr:
        cam = json.loads((bundle / "camera.json").read_text())
        intr_list = [(0, {k: cam[k] for k in ("width", "height", "fx", "fy", "cx", "cy")})]
        intr_list[0][1].update(model=cam["model"], distortion=cam.get("distortion", []))
    else:
        intr_list = [(m.t_ns, _intrinsics_dict(m)) for m in intr]
    it = [t for t, _ in intr_list]
    frames = [f for f in s.get("/frame_index") if f.keyframe and f.file]
    q_ns = [f.t_ns for f in frames]
    res = geometry.interpolate_at(
        [p.t_ns for p in poses],
        [[p.tx, p.ty, p.tz] for p in poses],
        [[p.qw, p.qx, p.qy, p.qz] for p in poses],
        q_ns,
        max_gap_ns,
    )
    out, skipped = [], 0
    for f, r in zip(frames, res, strict=True):
        if r is None:
            skipped += 1
            continue
        k = intr_list[max(0, bisect.bisect_right(it, f.t_ns) - 1)][1]
        out.append({"t_ns": f.t_ns, "file": f.file, "t": r[0], "q": r[1], "K": k})
    return {"frames": out, "skipped": skipped}


def export_colmap(
    bundle: Path, out: Path, max_gap_ns: int = 100_000_000, image_ids: dict | None = None
) -> dict:
    """image_ids: optional name -> IMAGE_ID map (e.g. from a COLMAP database) to reuse its IDs."""
    kp = load_keyframe_poses(bundle, max_gap_ns)
    out.mkdir(parents=True, exist_ok=True)
    cams: dict[tuple, int] = {}
    cam_lines, img_lines = [], []
    for i, f in enumerate(kp["frames"], start=1):
        c = geometry.to_colmap(f["t"], f["q"], f["K"])
        key = (c["model"], c["width"], c["height"], *c["params"])
        if key not in cams:
            cams[key] = len(cams) + 1
            params = " ".join(repr(float(p)) for p in c["params"])
            cam_lines.append(f"{cams[key]} {c['model']} {c['width']} {c['height']} {params}")
        vals = " ".join(repr(float(v)) for v in (*c["qvec_wxyz"], *c["tvec"]))
        iid = image_ids[f["file"]] if image_ids else i
        img_lines.append(f"{iid} {vals} {cams[key]} {f['file']}")
        img_lines.append("")
    (out / "cameras.txt").write_text(
        "# Camera list: CAMERA_ID MODEL WIDTH HEIGHT PARAMS[]\n" + "\n".join(cam_lines) + "\n"
    )
    (out / "images.txt").write_text(
        "# IMAGE_ID QW QX QY QZ TX TY TZ CAMERA_ID NAME (cam_from_world)\n"
        "# POINTS2D[] as (X, Y, POINT3D_ID)\n" + "\n".join(img_lines) + "\n"
    )
    (out / "points3D.txt").write_text("# 3D point list (empty: known-pose export)\n")
    return {"images": len(kp["frames"]), "cameras": len(cams), "skipped": kp["skipped"]}
