"""`ckit quality-report`: keyframe density, rejection histogram and pycolmap registration rate of
the keyframes the core selector would keep for a bundle."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

from ckit.replay.driver import replay


def registration_rate(
    bundle: Path, files: list[str], work: Path | None = None, num_threads: int | None = None
) -> dict:
    """Incremental SfM (pycolmap) on the given images only; registered / given.
    num_threads=None keeps the defaults; an int pins extraction, matching and mapping."""
    import pycolmap

    pycolmap.logging.minloglevel = 2
    pycolmap.logging.logtostderr = True
    tmp = Path(tempfile.mkdtemp(prefix="ckit_qr_")) if work is None else work
    try:
        db = tmp / "db.db"
        if db.exists():
            db.unlink()
        ext = pycolmap.FeatureExtractionOptions()
        ext.num_threads = 4 if num_threads is None else num_threads
        match_kw = {}
        if num_threads is not None:  # default call path stays exactly as before
            match = pycolmap.FeatureMatchingOptions()
            match.num_threads = num_threads
            match_kw["matching_options"] = match
        pycolmap.extract_features(
            db,
            bundle,
            image_names=files,
            camera_mode=pycolmap.CameraMode.SINGLE,
            extraction_options=ext,
            device=pycolmap.Device.cpu,
        )
        if len(files) <= 80:
            pycolmap.match_exhaustive(db, device=pycolmap.Device.cpu, **match_kw)
        else:
            pair = pycolmap.SequentialPairingOptions()
            pair.overlap = 15
            pycolmap.match_sequential(
                db, pairing_options=pair, device=pycolmap.Device.cpu, **match_kw
            )
        opts = pycolmap.IncrementalPipelineOptions()
        opts.random_seed = 0  # deterministic mapping where pycolmap allows it
        if num_threads is not None:
            opts.num_threads = num_threads
            opts.mapper.num_threads = num_threads
        recs = pycolmap.incremental_mapping(db, bundle, tmp / "sparse", options=opts)
        best = max((r.num_reg_images() for r in recs.values()), default=0)
        return {
            "images": len(files),
            "registered": best,
            "registration_rate": round(best / len(files), 4) if files else 0.0,
            "models": len(recs),
        }
    finally:
        if work is None:
            shutil.rmtree(tmp, ignore_errors=True)


def quality_report(
    bundle: Path, config: dict | None = None, register: bool = True, uniform: bool = False
) -> dict:
    """uniform=True also registers the same number of frames picked at a fixed interval (the
    rejected alternative), so the report shows whether selection beats fixed spacing."""
    rep = replay(bundle, ["quality"], sink_config={"quality": dict(config or {}, emit_frames=1)})[
        "sinks"
    ]["quality"]
    files = {f["frame_id"]: None for f in rep["per_frame"] if f["keyframe"]}
    # Map selected frame ids to the bundle's JPEG files.
    from ckit.core import load

    all_frames = load().bundle.read_streams(str(bundle / "streams.mcap"))["frames"]
    for fr in all_frames:
        if fr["frame_id"] in files and fr["file"]:
            files[fr["frame_id"]] = fr["file"]
    sel = [f for f in files.values() if f]
    out = {k: v for k, v in rep.items() if k not in ("per_frame", "config")}
    out["bundle"] = bundle.name
    if register and len(sel) >= 3:
        out["sfm"] = registration_rate(bundle, sel)
        if uniform:
            pool = [f["file"] for f in all_frames if f["file"]]
            step = len(pool) / len(sel)
            uni = [pool[int(i * step)] for i in range(len(sel))]
            out["sfm_uniform_same_count"] = registration_rate(bundle, uni)
    return out
