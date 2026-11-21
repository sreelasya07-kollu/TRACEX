"""Reader for the on-disk SpatialMap (docs/INTERFACES.md §6). Pure formats: pydantic + stdlib +
numpy only, so Part 4 (`recon/`) can import it without importing `spatial/`.

Verification is part of loading: every file listed in the manifest is hashed, and files
present on disk but missing from the manifest are reported."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path

from formats.models import (
    CamerasFile,
    PlanesFile,
    RoomModel,
    SpatialMapManifest,
    SpatialQuality,
)

MANIFEST = "manifest.json"


class SpatialMapError(RuntimeError):
    pass


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


@dataclass
class SpatialMap:
    root: Path
    manifest: SpatialMapManifest
    cameras: CamerasFile
    planes: PlanesFile
    room: RoomModel
    quality: SpatialQuality

    @property
    def fused_cloud(self) -> Path:
        return self.root / "fused_cloud.ply"

    @property
    def tsdf_mesh(self) -> Path:
        return self.root / "tsdf_mesh.ply"

    @property
    def sparse_dir(self) -> Path:
        return self.root / "sparse"

    @property
    def images_dir(self) -> Path:
        return self.root / "images"

    def depth_png(self, frame_id: str) -> Path:
        return self.root / "depth" / f"{frame_id}.png"


def verify(root: Path) -> list[str]:
    """Returns a list of problems (empty = consistent)."""
    root = Path(root)
    man = SpatialMapManifest.model_validate_json((root / MANIFEST).read_text())
    problems = []
    for rel, digest in man.files.items():
        p = root / rel
        if not p.is_file():
            problems.append(f"missing: {rel}")
        elif _sha256(p) != digest:
            problems.append(f"hash mismatch: {rel}")
    on_disk = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()} - {MANIFEST}
    problems += [f"not in manifest: {r}" for r in sorted(on_disk - set(man.files))]
    return problems


def load_spatial_map(root: Path, verify_hashes: bool = True) -> SpatialMap:
    root = Path(root)
    if not (root / MANIFEST).is_file():
        raise SpatialMapError(f"{root}: no {MANIFEST}")
    if verify_hashes and (problems := verify(root)):
        raise SpatialMapError(f"{root}: " + "; ".join(problems[:5]))
    man = SpatialMapManifest.model_validate_json((root / MANIFEST).read_text())
    if man.version.split(".")[0] != "0":
        raise SpatialMapError(f"unsupported SpatialMap version {man.version}")
    return SpatialMap(
        root=root,
        manifest=man,
        cameras=CamerasFile.model_validate_json((root / "cameras" / "cameras.json").read_text()),
        planes=PlanesFile.model_validate_json((root / "planes.json").read_text()),
        room=RoomModel.model_validate_json((root / "room.json").read_text()),
        quality=SpatialQuality.model_validate_json((root / "quality.json").read_text()),
    )
