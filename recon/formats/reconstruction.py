"""Reader/verifier for the on-disk Reconstruction folder (docs/INTERFACES.md §7)."""

from __future__ import annotations

from pathlib import Path

from formats.models import ReconstructionManifest
from formats.util import sha256_file

MANIFEST = "manifest.json"


def verify_reconstruction(root: Path) -> list[str]:
    root = Path(root)
    man = ReconstructionManifest.model_validate_json((root / MANIFEST).read_text())
    problems = []
    for rel, digest in man.files.items():
        p = root / rel
        if not p.is_file():
            problems.append(f"missing: {rel}")
        elif sha256_file(p) != digest:
            problems.append(f"hash mismatch: {rel}")
    on_disk = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()} - {MANIFEST}
    problems += [f"not in manifest: {r}" for r in sorted(on_disk - set(man.files))]
    for name, e in man.outputs.items():
        problems += [f"{name}: file not in manifest: {f}" for f in e.files if f not in man.files]
    return problems


def load_reconstruction(root: Path, verify: bool = True) -> ReconstructionManifest:
    if verify and (problems := verify_reconstruction(root)):
        raise RuntimeError(f"{root}: " + "; ".join(problems[:5]))
    return ReconstructionManifest.model_validate_json((Path(root) / MANIFEST).read_text())
