"""Phase 10: Part 4 end-to-end: SpatialMap -> sparse -> dense -> mesh -> texture -> (splat) ->
Reconstruction folder with manifest, hashes and eval metrics (docs/INTERFACES.md §7)."""

from __future__ import annotations

import json
import shutil
import time
from dataclasses import asdict
from datetime import UTC, datetime
from pathlib import Path

from formats.models import OutputEntry, ReconstructionManifest
from formats.spatial_map import MANIFEST as SM_MANIFEST
from formats.spatial_map import load_spatial_map
from formats.util import PeakRam, sha256_file, versions
from recon.dense import DenseParams, run_dense_mesh_texture
from recon.evaluate import heldout_render_metrics, split_names
from recon.sparse import SparseScene, check_scene
from recon.splat.train import GpuRequired, SplatJob, cuda_available, train_splat


def run_reconstruction(
    spatial_map: Path,
    out: Path,
    dense: DenseParams | None = None,
    *,
    splat: str = "auto",  # auto: train if CUDA is present, else record "skipped"; yes | no
    splat_steps: int = 7000,
    heldout_every: int = 8,  # 0 = use every image (no held-out metrics)
    seed: int = 0,
) -> Path:
    if out.exists():
        raise FileExistsError(out)
    dense = dense or DenseParams()
    sm = load_spatial_map(spatial_map)  # verifies every hash of the input
    scene = SparseScene.from_spatial_map(sm)
    out.mkdir(parents=True)
    timings: dict[str, float] = {}
    outputs: dict[str, OutputEntry] = {}
    eval_metrics: dict = {}
    names = sorted(p.name for p in sm.images_dir.glob("*.jpg"))
    train_names, held = split_names(names, heldout_every) if heldout_every else (names, [])

    with PeakRam() as ram:
        # sparse: the model Part 3 refined, unchanged
        t0 = time.perf_counter()
        chk = check_scene(scene, n_expected=len(names))
        shutil.copytree(sm.sparse_dir, out / "sparse")
        timings["sparse"] = time.perf_counter() - t0
        outputs["sparse"] = OutputEntry(
            status="ok",
            files=[f"sparse/{n}" for n in sorted(p.name for p in sm.sparse_dir.iterdir())],
            metrics=chk,
        )

        # dense + mesh + texture (OpenMVS)
        try:
            run = run_dense_mesh_texture(scene, out, dense, train_names if heldout_every else None)
            timings |= {f"openmvs_{k}": v for k, v in run["timings_s"].items()}
            dense_files = sorted(
                str(p.relative_to(out)) for p in (out / "dense").rglob("*") if p.is_file()
            )
            mesh_files = sorted(
                str(p.relative_to(out)) for p in (out / "mesh").rglob("*") if p.is_file()
            )
            outputs["dense"] = OutputEntry(
                status="ok", files=dense_files, metrics={"points": run["stats"]["dense_points"]}
            )
            mesh_metrics = {k: run["stats"][k] for k in ("mesh_vertices", "mesh_faces", "glb")}
            if held and "textured_obj" in run["outputs"]:
                t0 = time.perf_counter()
                hm = heldout_render_metrics(
                    out / run["outputs"]["textured_obj"],
                    scene,
                    held,
                    1 / 2**dense.resolution_level,
                    out / "eval" / "mesh_heldout",
                )
                timings["eval_mesh_heldout"] = time.perf_counter() - t0
                mesh_metrics["heldout"] = {k: v for k, v in hm.items() if k != "per_image"}
                eval_metrics["mesh"] = hm
            outputs["mesh"] = OutputEntry(status="ok", files=mesh_files, metrics=mesh_metrics)
        except Exception as e:  # noqa: BLE001 - recorded, not hidden
            outputs["dense"] = OutputEntry(status="failed", reason=f"{type(e).__name__}: {e}")
            outputs["mesh"] = OutputEntry(status="failed", reason=f"{type(e).__name__}: {e}")

        # splat: separate output, separate metrics, trained from the SpatialMap (not the mesh)
        if splat == "no":
            outputs["splat"] = OutputEntry(status="skipped", reason="disabled by --splat no")
        elif splat == "auto" and not cuda_available():
            outputs["splat"] = OutputEntry(
                status="skipped", reason="no CUDA device: splat training runs on the GPU machine"
            )
        else:
            try:
                job = SplatJob(
                    str(spatial_map),
                    str(out / "splat"),
                    steps=splat_steps,
                    seed=seed,
                    heldout_every=heldout_every or 8,
                )
                t0 = time.perf_counter()
                m = train_splat(job)
                timings["splat_train"] = time.perf_counter() - t0
                outputs["splat"] = OutputEntry(
                    status="ok",
                    files=sorted(
                        str(p.relative_to(out)) for p in (out / "splat").rglob("*") if p.is_file()
                    ),
                    metrics=m,
                )
                eval_metrics["splat"] = m
            except GpuRequired as e:
                if splat == "yes":
                    raise
                outputs["splat"] = OutputEntry(status="skipped", reason=str(e))

    (out / "eval").mkdir(exist_ok=True)
    (out / "eval" / "metrics.json").write_text(json.dumps(eval_metrics, indent=1, default=str))
    files = {
        str(p.relative_to(out)): sha256_file(p)
        for p in sorted(out.rglob("*"))
        if p.is_file() and p.name != "manifest.json"
    }
    man = ReconstructionManifest(
        created=datetime.now(UTC).isoformat(),
        spatial_map={
            "path": str(spatial_map),
            "manifest_sha256": sha256_file(spatial_map / SM_MANIFEST),
            "quality_warnings": sm.quality.warnings,
        },
        outputs=outputs,
        files=files,
        timings_s=timings,
        peak_ram_mb=ram.peak / 2**20,
        params={
            "dense": asdict(dense),
            "splat": splat,
            "heldout_every": heldout_every,
            "seed": seed,
        },
        versions=versions(),
    )
    (out / "manifest.json").write_text(man.model_dump_json(indent=1))
    return out
