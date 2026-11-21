"""Phase 10 (HANDOFF P10): Reconstruction packaging. light: manifest verification and schema.
heavy+longrun: `recon.cli run` end to end on the Replica SpatialMap (CPU; splat recorded as
skipped because this machine has no CUDA) and, when present, the ETH3D SpatialMap."""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import jsonschema
import pytest

from formats.models import OutputEntry, ReconstructionManifest
from formats.reconstruction import load_reconstruction, verify_reconstruction
from formats.util import sha256_file
from recon import openmvs

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "runs" / "test_results"


def _tiny(tmp: Path) -> Path:
    (tmp / "mesh").mkdir(parents=True)
    (tmp / "mesh" / "mesh.ply").write_bytes(b"ply")
    (tmp / "sparse").mkdir()
    (tmp / "sparse" / "cameras.bin").write_bytes(b"c")
    man = ReconstructionManifest(
        created="2026-11-14T00:00:00+00:00",
        spatial_map={"path": "x", "manifest_sha256": "0" * 64},
        outputs={
            "sparse": OutputEntry(status="ok", files=["sparse/cameras.bin"]),
            "mesh": OutputEntry(status="ok", files=["mesh/mesh.ply"], metrics={"mesh_faces": 1}),
            "splat": OutputEntry(status="skipped", reason="no CUDA device"),
        },
        files={
            "mesh/mesh.ply": sha256_file(tmp / "mesh" / "mesh.ply"),
            "sparse/cameras.bin": sha256_file(tmp / "sparse" / "cameras.bin"),
        },
        timings_s={"sparse": 0.1},
        peak_ram_mb=1.0,
        params={},
        versions={},
    )
    (tmp / "manifest.json").write_text(man.model_dump_json(indent=1))
    return tmp


@pytest.mark.light
def test_manifest_schema_verify_and_tamper(tmp_path):
    root = _tiny(tmp_path / "r")
    jsonschema.validate(
        json.loads((root / "manifest.json").read_text()),
        json.loads((ROOT / "spec/reconstruction/manifest.schema.json").read_text()),
    )
    man = load_reconstruction(root)
    assert man.outputs["splat"].status == "skipped" and man.outputs["splat"].files == []
    shutil.copytree(root, tmp_path / "bad")
    (tmp_path / "bad" / "mesh" / "mesh.ply").write_bytes(b"x")
    (tmp_path / "bad" / "stray.txt").write_text("s")
    probs = verify_reconstruction(tmp_path / "bad")
    assert any("hash mismatch: mesh/mesh.ply" in p for p in probs)
    assert any("not in manifest: stray.txt" in p for p in probs)
    with pytest.raises(ValueError):  # extra fields are forbidden
        ReconstructionManifest.model_validate({**man.model_dump(), "surprise": 1})


def _e2e(sm: Path, out: Path, tag: str, **kw):
    from recon.dense import DenseParams
    from recon.package import run_reconstruction

    if not out.exists():
        run_reconstruction(sm, out, DenseParams(**kw), splat="auto", heldout_every=8)
    man = load_reconstruction(out)  # verifies every hash
    res = {
        "outputs": {
            k: {"status": v.status, "reason": v.reason, "metrics": v.metrics}
            for k, v in man.outputs.items()
        },
        "timings_s": man.timings_s,
        "peak_ram_mb": man.peak_ram_mb,
        "n_files": len(man.files),
    }
    (RESULTS / f"phase10_{tag}.json").write_text(json.dumps(res, indent=1, default=str))
    return man


@pytest.mark.heavy
@pytest.mark.longrun(hours=0.3)
def test_end_to_end_replica():
    sm = ROOT / "runs/dev_p6_replica/spatial_map"
    if not sm.exists() or not openmvs.available():
        pytest.skip("needs runs/dev_p6_replica/spatial_map and docker image room-openmvs:2.4.0")
    man = _e2e(sm, ROOT / "runs/dev_p10_replica", "replica", resolution_level=0)
    assert man.outputs["sparse"].metrics["reg_ratio"] >= 0.95
    assert man.outputs["dense"].status == "ok" and man.outputs["mesh"].status == "ok"
    assert man.outputs["mesh"].metrics["mesh_faces"] > 10_000
    assert man.outputs["mesh"].metrics["heldout"]["mean_coverage"] > 0.3
    sp = man.outputs["splat"]
    assert sp.status == "skipped" and "CUDA" in sp.reason and sp.files == []  # never faked
