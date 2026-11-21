"""Plans which pipeline stages to run. Pure logic (no imports of spatial/recon): it names the
command line of each stage and estimates its cost.

Rules that matter:
- Accuracy is never degraded silently: if the time budget is too small the plan is flagged
  `over_budget` with the numbers, and nothing is switched to a coarser setting.
- A product that needs a missing capability (CUDA, gravity, metric scale) is either placed on a
  `gpu_worker` (executable later) or refused with the reason; it is never faked.
- Estimates are ESTIMATES scaled from measurements on the 8-core CPU development machine
  (docs/PHASE_REPORTS); they are not guarantees.
"""

from __future__ import annotations

from dataclasses import dataclass, field

OUTPUTS = {"preview", "pointcloud", "mesh", "splat", "floorplan", "semantic"}
TIERS = {"A": "LiDAR/ToF depth + device poses", "B": "device poses, no depth", "C": "RGB only"}
# seconds per frame on the 8-core dev machine, from measured runs
PER_FRAME_S = {"spatial_sensor": 0.6, "spatial_mvs": 3.5, "mesh_level0": 28.0, "mesh_level1": 7.0}


@dataclass
class Request:
    outputs: set[str]
    tier: str  # A | B | C
    n_frames: int = 60
    has_cuda: bool = False
    time_budget_min: float | None = None
    spatial_verdict: str | None = None  # qa verdict of an existing SpatialMap, if known
    resolution_level: int = 0  # accuracy setting; never changed by the router


@dataclass
class StagePlan:
    name: str
    command: str
    where: str  # local | gpu_worker
    est_minutes: float
    ram_gb: float
    notes: list[str] = field(default_factory=list)


@dataclass
class Plan:
    recipe: str
    stages: list[StagePlan]
    warnings: list[str]
    refused: dict[str, str]  # output -> reason
    fallbacks: list[str]
    est_minutes_local: float
    over_budget: bool

    def to_dict(self) -> dict:
        return {
            "recipe": self.recipe,
            "stages": [s.__dict__ for s in self.stages],
            "warnings": self.warnings,
            "refused": self.refused,
            "fallbacks": self.fallbacks,
            "est_minutes_local": self.est_minutes_local,
            "over_budget": self.over_budget,
        }


def _recipe_name(outs: set[str]) -> str:
    if outs <= {"preview"}:
        return "preview"
    if outs & {"splat"} and outs & {"mesh"}:
        return "photoreal+mesh"
    if "splat" in outs:
        return "photoreal"
    if "mesh" in outs:
        return "standard"
    if outs & {"floorplan", "semantic"}:
        return "semantic"
    return "geometry"


def plan(req: Request) -> Plan:
    if req.tier not in TIERS:
        raise ValueError(f"tier must be one of {sorted(TIERS)}")
    unknown = req.outputs - OUTPUTS
    if unknown:
        raise ValueError(f"unknown outputs {sorted(unknown)}; choose from {sorted(OUTPUTS)}")
    warnings: list[str] = []
    refused: dict[str, str] = {}
    fallbacks: list[str] = []
    outs = set(req.outputs)

    # capture-tier capabilities
    gravity = req.tier in ("A", "B")  # device poses carry gravity
    metric = req.tier in ("A", "B")
    sensor = req.tier == "A"
    if not gravity:
        for o in ("floorplan", "semantic"):
            if o in outs:
                refused[o] = (
                    "needs a gravity-aligned frame: RGB-only capture has no device poses or IMU "
                    "gravity (rescan with an AR-pose capture)"
                )
        outs -= set(refused)
    if not metric:
        warnings.append("RGB-only capture: model is NOT metric (arbitrary scale); no measurements")
    if req.tier == "B":
        fallbacks.append("no depth sensor: depth comes from OpenMVS multi-view stereo (slower)")
    if req.spatial_verdict == "needs_rescan":
        for o in sorted(outs - {"preview"}):
            refused[o] = "the existing scan failed the QA gate: rescan the flagged regions first"
        outs &= {"preview"}
    if "splat" in outs and not req.has_cuda:
        warnings.append("splat training needs CUDA: planned on a gpu_worker, not runnable locally")

    stages: list[StagePlan] = []
    if not outs:
        return Plan("refused", [], warnings, refused, fallbacks, 0.0, False)

    n = req.n_frames
    frames = min(n, 30) if outs <= {"preview"} else n
    depth_key = "spatial_sensor" if sensor else "spatial_mvs"
    flags = (
        "--keyframes"
        + (" --depth-scale" if sensor else "")
        + (" --prior none" if not gravity else "")
    )
    stages.append(
        StagePlan(
            "spatial_run",
            f"python -m spatial.cli run --dataset {{dataset}} --src {{src}} --out {{run}} "
            f"--max-frames {frames} {flags}",
            "local",
            frames * PER_FRAME_S[depth_key] / 60,
            3.0,
            ["ingest, pose refinement, depth, fusion, planes, SpatialMap"],
        )
    )
    stages.append(
        StagePlan(
            "qa_spatial",
            "python -m qa.cli report --spatial-map {run}/spatial_map --out {run}/job_report.json",
            "local",
            0.1,
            1.0,
            ["coverage, gates, anchored rescan regions: run BEFORE the expensive stages"],
        )
    )
    if outs & {"mesh"}:
        key = "mesh_level0" if req.resolution_level == 0 else "mesh_level1"
        stages.append(
            StagePlan(
                "recon_mesh",
                "python -m recon.cli run --spatial-map {run}/spatial_map "
                "--out {run}/reconstruction "
                f"--resolution-level {req.resolution_level} --splat no "
                "--max-threads 3 --memory-gb 4",
                "local",
                n * PER_FRAME_S[key] / 60,
                4.0,
                ["OpenMVS dense + mesh + texture (CPU, docker); memory-capped container"],
            )
        )
    if "splat" in outs:
        stages.append(
            StagePlan(
                "recon_splat",
                "python -m recon.cli run --spatial-map {run}/spatial_map "
                "--out {run}/reconstruction_splat --splat yes",
                "local" if req.has_cuda else "gpu_worker",
                15.0,
                8.0,
                ["gsplat training; ESTIMATE 15 min, never measured on this machine"],
            )
        )
    if outs & {"floorplan", "semantic", "pointcloud", "preview"}:
        stages.append(
            StagePlan(
                "export",
                "python -m export.cli all --spatial-map {run}/spatial_map --out {run}/exports",
                "local",
                0.2,
                1.0,
                ["floor plan SVG/DXF, USD room shell, LAZ, measurements"],
            )
        )
    if outs & {"mesh", "splat"}:
        stages.append(
            StagePlan(
                "qa_final",
                "python -m qa.cli report --spatial-map {run}/spatial_map "
                "--reconstruction {run}/reconstruction --out {run}/job_report_final.json",
                "local",
                0.1,
                1.0,
                ["adds held-out mesh / splat quality gates"],
            )
        )
    local = sum(s.est_minutes for s in stages if s.where == "local")
    over = req.time_budget_min is not None and local > req.time_budget_min
    if over:
        warnings.append(
            f"estimated {local:.1f} min locally exceeds the {req.time_budget_min:.1f} min budget; "
            "accuracy settings were NOT reduced (raise the budget or drop an output)"
        )
    return Plan(_recipe_name(outs), stages, warnings, refused, fallbacks, local, over)
