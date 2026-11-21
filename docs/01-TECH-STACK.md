# Tech Stack

> Every tool gets one block: WHAT it is, WHY we use it, HOW we use it, alternatives, ADR.
> Versions are not copied here: `pyproject.toml` holds the constraints and `uv.lock` the
> resolved versions. Licences live in [MODELS.md](MODELS.md).
> Adding, replacing or re-pinning a tool needs a record in [decisions/](decisions/) first.
> Tools adopted before 2026-11-17 have no ADR; their record is the PROGRESS.md
> "Decisions and findings log" and HANDOFF.md §3 "Known library facts".

## Summary table
| Layer | Technology | Version | Status |
|---|---|---|---|
| Language / env | Python + uv project, torch extra `cpu`/`cu126`/`cu130` | [pyproject.toml](../pyproject.toml), `uv.lock` | Approved |
| Geometry / fusion | Open3D | pinned, [pyproject.toml](../pyproject.toml) | Approved |
| SfM / poses | COLMAP via pycolmap | pinned, [pyproject.toml](../pyproject.toml) | Approved |
| Dense / mesh / texture | OpenMVS (docker `room-openmvs`) | [docker/openmvs/Dockerfile](../docker/openmvs/Dockerfile) | Approved |
| Splatting | gsplat + PyTorch | pinned, [pyproject.toml](../pyproject.toml) | Approved (GPU runs unverified) |
| Mono depth | Depth-Anything-V2 via transformers | model ids in [MODELS.md](MODELS.md) | Approved (fallback only) |
| Formats | pydantic models + JSON Schema | [pyproject.toml](../pyproject.toml) | Approved |
| CLI | typer | [pyproject.toml](../pyproject.toml) | Approved |
| Mesh / export IO | trimesh, plyfile, ezdxf, laspy | [pyproject.toml](../pyproject.toml) | Approved |
| Image / numeric | numpy, scipy, opencv-python, torchmetrics | [pyproject.toml](../pyproject.toml) | Approved |
| Evaluation | ETH3D multi-view evaluation (docker `room-eth3d-eval`) | [docker/eth3d-eval/Dockerfile](../docker/eth3d-eval/Dockerfile) | Approved (tests only) |
| Testing / lint | pytest (markers), ruff | `[dependency-groups] dev` in [pyproject.toml](../pyproject.toml) | Approved |
| CI | GitHub Actions | [.github/workflows/](../.github/workflows/) | Approved, [ADR 002](decisions/002-github-actions-ci.md) |

---

## Python + uv
- **What:** Python interpreter and the uv package manager with a lockfile.
- **Why:** reproducible pinned environment across the CPU laptop and the GPU machine
  (HANDOFF.md §3); the torch flavour is selected by an extra.
- **How:** `uv sync --extra cpu` (or `cu126` / `cu130`; always pass one, HANDOFF.md §3).
  Python range is in `requires-python` in `pyproject.toml` (bounded by Open3D wheels).
- **Alternatives:** none recorded.
- **ADR:** none (pre-ADR).

## Open3D
- **What:** 3D data library (point clouds, RANSAC planes, TSDF, Poisson, ray casting).
- **Why:** mature library, required by HANDOFF.md §2 rule 3; CPU capable.
- **How:** `spatial/fusion/` (TSDF, cleaning), `spatial/room/` (planes), `recon/dense.py`
  (Poisson fallback), `recon/render.py`, `eval/`, `export/pointcloud.py`.
- **Alternatives:** newer Open3D release rejected (TSDF returns empty meshes); see PROGRESS.md
  "Decisions and findings log" and the comment in `pyproject.toml`.
- **ADR:** none (pre-ADR). Upgrading needs an ADR and the `tests/test_env.py` TSDF test first.

## COLMAP / pycolmap
- **What:** structure-from-motion and bundle adjustment, Python bindings.
- **Why:** mature SfM (HANDOFF.md §2 rule 3); CPU wheel is enough for SIFT + BA.
- **How:** `spatial/sfm/refine.py` (pose refinement, SfM fallback), `recon/sparse.py`
  (Part 4 sparse hand-over), `recon/evaluate.py`.
- **Alternatives:** CUDA build / PatchMatch stereo (HANDOFF.md §3); global mapper is
  experimental (PROGRESS.md decisions log). GLOMAP / VGGT: C-SplatForge §5, not integrated.
- **ADR:** none (pre-ADR).

## OpenMVS (docker)
- **What:** multi-view stereo: dense cloud, mesh reconstruction, refinement, texturing.
- **Why:** mature CPU dense path (HANDOFF.md §2 rule 3, §5 P8); MVS depth for Phase 3.
- **How:** separate container only, never linked (licence: [MODELS.md](MODELS.md)).
  Wrappers in `recon/openmvs.py`, `recon/dense.py`, `spatial/depth/mvs.py`.
  Build command: HANDOFF.md §3. The OpenMVS version and image tag are owned by
  [docker/openmvs/Dockerfile](../docker/openmvs/Dockerfile).
- **Alternatives:** the 2019 Docker Hub image (HANDOFF.md §3); COLMAP PatchMatch (needs CUDA).
- **ADR:** none (pre-ADR).

## gsplat + PyTorch
- **What:** CUDA Gaussian-splat rasterizer and the tensor library it runs on.
- **Why:** permissive splat trainer (C-SplatForge §11; licences in [MODELS.md](MODELS.md)).
- **How:** `recon/splat/train.py` wraps `gsplat.rasterization` + `DefaultStrategy`;
  `recon/splat/data.py` and `plyio.py` run on CPU. Training needs CUDA (HANDOFF.md §2 rule 5).
- **Alternatives:** nerfstudio `splatfacto` (excluded: conflicting pins, PROGRESS.md decisions
  log); original Inria 3DGS code (licence, C-SplatForge §11).
- **ADR:** none (pre-ADR).

## Depth-Anything-V2 (transformers)
- **What:** monocular depth model loaded through Hugging Face transformers.
- **Why:** depth source when no sensor or MVS depth exists; fallback only (PROGRESS.md P3).
- **How:** `spatial/depth/sources.py`. Model ids, default and licence split: [MODELS.md](MODELS.md).
- **Alternatives:** Depth Pro, MoGe (C-SplatForge §5 P4), not integrated.
- **ADR:** none (pre-ADR).

## pydantic + jsonschema
- **What:** typed models for every on-disk format; JSON Schema validation.
- **Why:** one executable definition per format (docs/INTERFACES.md intro).
- **How:** models in `formats/`; schemas exported to `spec/` by
  `python scripts/export_schemas.py [--check]`; tests validate against them.
- **Alternatives:** none recorded.
- **ADR:** none (pre-ADR).

## typer
- **What:** CLI framework.
- **How:** `spatial/cli.py`, `recon/cli.py`, `qa/cli.py`, `router/cli.py`, `export/cli.py`
  (`python -m <pkg>.cli --help`).
- **Why / alternatives:** none recorded. **ADR:** none (pre-ADR).

## trimesh, plyfile, ezdxf, laspy
- **What:** mesh IO + GLB (trimesh), raw PLY IO (plyfile), DXF (ezdxf), LAZ (laspy).
- **Why:** plyfile handles quad faces and float32 PLYs that Open3D mishandles (PROGRESS.md
  decisions log); ezdxf / laspy serve the exports in docs/PLAN.md §4 item 4.
- **How:** `recon/dense.py`, `recon/render.py` (trimesh); `recon/splat/plyio.py`, `eval/`
  (plyfile); `export/floorplan.py` (ezdxf); `export/pointcloud.py` (laspy).
- **Alternatives:** none recorded. **ADR:** none (pre-ADR).

## numpy, scipy, opencv-python, torchmetrics, psutil, matplotlib
- **What:** numerics, image processing, image metrics (PSNR/SSIM/LPIPS), RAM sampling, figures.
- **How:** throughout `spatial/`, `recon/`, `eval/`, `qa/`; psutil in `formats/util.py` and
  `tests/conftest.py` (peak RAM; PROGRESS.md decisions log); matplotlib in `benchmarks/`.
- **Alternatives:** none recorded. **ADR:** none (pre-ADR).

## ETH3D multi-view evaluation (docker)
- **What:** official ETH3D scoring tool.
- **Why:** accepted benchmark metric for HANDOFF.md §5 P4/P8.
- **How:** `eval/eth3d.py` (converts to float32 PLY first: PROGRESS.md decisions log).
  Build: `docker build -t room-eth3d-eval docker/eth3d-eval`.
- **Alternatives:** none recorded. **ADR:** none (pre-ADR).

## pytest + ruff
- **What:** test runner with resource markers; linter/formatter.
- **How:** markers (`light`, `heavy`, `gpu`, `highmem`, `largedisk`, `longrun`, `arkit`) are
  defined in `pyproject.toml`. `uv run pytest -m light`, `uv run ruff check`.
- **Alternatives:** none recorded. **ADR:** none (pre-ADR).

## GitHub Actions
- **What:** hosted CI.
- **How:** [.github/workflows/charter-check.yml](../.github/workflows/charter-check.yml) runs
  `bash scripts/verify-charter.sh`. It runs no project tests yet ([04-TASKS](04-TASKS.md)).
- **ADR:** [002-github-actions-ci](decisions/002-github-actions-ci.md).

---

## Environment variables
| Name | Purpose | Where set | Secret? |
|---|---|---|---|
| `RUN_LONGRUN` | `=1` enables `longrun` tests (same as `--run-longrun`) | shell | No |

## External services / APIs
| Service | Purpose | Account owner | Docs link |
|---|---|---|---|
| Hugging Face Hub | model weight download | none (anonymous) | [MODELS.md](MODELS.md) |
| Dataset hosts (ETH3D, TUM, Replica, NYU, ARKitScenes) | evaluation data | none | [data/DATASETS.md](../data/DATASETS.md), `data/fetch.sh`, `scripts/heavy/fetch_datasets.sh`; [heavy-compute/INVENTORY.md](heavy-compute/INVENTORY.md) |
