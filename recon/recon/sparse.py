"""Phase 7: sparse model hand-over. Loads a COLMAP model (from a SpatialMap, or any workspace
with `sparse/0` + `images/`), checks it, and converts it for OpenMVS (InterfaceCOLMAP)."""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from pathlib import Path

try:
    import pycolmap
except (ImportError, RuntimeError):
    pycolmap = None

from formats.spatial_map import SpatialMap
from recon.openmvs import run_tool

BIN_FILES = ("cameras.bin", "images.bin", "points3D.bin")


@dataclass
class SparseScene:
    model_dir: Path  # folder with cameras.bin / images.bin / points3D.bin
    images_dir: Path
    source: str

    @classmethod
    def from_spatial_map(cls, sm: SpatialMap) -> SparseScene:
        return cls(sm.sparse_dir, sm.images_dir, f"spatial_map:{sm.root}")

    @classmethod
    def from_workspace(cls, ws: Path) -> SparseScene:
        return cls(ws / "sparse" / "0", ws / "images", f"workspace:{ws}")


def check_scene(scene: SparseScene, n_expected: int | None = None) -> dict:
    """Loads the model and reports registration + reprojection. Raises if images are missing."""
    for f in BIN_FILES:
        if not (scene.model_dir / f).is_file():
            raise FileNotFoundError(scene.model_dir / f)
    rec = pycolmap.Reconstruction(scene.model_dir)
    reg = [im for im in rec.images.values() if im.has_pose]
    missing = [im.name for im in reg if not (scene.images_dir / im.name).is_file()]
    if missing:
        raise FileNotFoundError(f"{len(missing)} registered images missing, e.g. {missing[0]}")
    n = n_expected if n_expected is not None else len(rec.images)
    return {
        "registered": len(reg),
        "n_images": n,
        "reg_ratio": len(reg) / max(1, n),
        "n_points3D": rec.num_points3D(),
        "mean_reproj_px": float(rec.compute_mean_reprojection_error()),
        "mean_track_length": float(rec.compute_mean_track_length()),
    }


def export_for_openmvs(scene: SparseScene, out: Path, image_names: list[str] | None = None) -> Path:
    """COLMAP model -> out/scene.mvs. `image_names` restricts the model (held-out evaluation):
    the images not listed are removed from the exported model."""
    out.mkdir(parents=True, exist_ok=True)
    stage = out / "colmap" / "sparse"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    if image_names is None:
        for f in BIN_FILES:
            shutil.copy(scene.model_dir / f, stage / f)
    else:
        rec = pycolmap.Reconstruction(scene.model_dir)
        keep = set(image_names)
        drop = [im.frame_id for im in rec.images.values() if im.name not in keep]
        for fid in drop:  # pycolmap 4.x: images are registered through their frame
            rec.deregister_frame(fid)
        rec.write(stage)
    run_tool(
        "InterfaceCOLMAP",
        ["-i", "/out/colmap", "--image-folder", "/images/", "-o", "scene.mvs"],
        out,
        images=scene.images_dir,
    )
    return out / "scene.mvs"
