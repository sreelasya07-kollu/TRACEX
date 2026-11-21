# Room Scanner Parts 3 & 4: handoff for the GPU machine

This file is for a new Claude Code session (and the people working with it) continuing
Room Scanner **Part 3 (Spatial Understanding)** and **Part 4 (3D Reconstruction)** on a
machine with an NVIDIA GPU. Read the whole file before touching code.

## 1. State on handoff (2026-11-12)

| Item | State |
|---|---|
| `capturekit/` (Project A: phone capture SDK) | Skeleton only (see `capturekit/README.md`). **Not your scope.** Don't modify it. |
| Scan Bundle spec (`capturekit/spec/scanbundle/`) | **Not written yet**: no schemas, no `coordinates.md`. Part 3 must not wait for it (see §4.1). |
| `reconstruction/` (this folder) | uv workspace with two empty packages plus a pinned environment, Dockerfiles and a data fetch script. **No algorithm code yet.** |
| Git | Nothing committed yet (the user asked for that). Commit only when the user asks. |

What already exists in `reconstruction/`:

```
pyproject.toml            uv workspace root; torch flavour chosen by extra: cpu | cu126 | cu130
uv.lock                   full lock (all flavours)
packages/room_spatial/    Part 3 package (src layout, only __init__.py)
packages/room_recon/      Part 4 package (src layout, only __init__.py)
docker/openmvs/           OpenMVS v2.4.0 CPU build (Ubuntu 24.04 deps, no vcpkg)
docker/eth3d-eval/        ETH3D official multi-view evaluation tool (scoring only)
scripts/fetch_test_data.sh  ETH3D "office" + TUM RGB-D fr1/xyz into data/ (gitignored)
```

## 2. Rules from the project owner (non-negotiable)

1. Part 3 and Part 4 are **independent modules** with documented interfaces. `room_recon`
   never imports `room_spatial` (or the reverse). They exchange data only through the
   on-disk `SpatialMap` format (§4.2).
2. **Gaussian Splats and textured meshes are separate outputs** of Part 4 (separate
   folders, separate manifest entries, neither derived from the other).
3. Reuse mature libraries: COLMAP/pycolmap, Open3D, OpenMVS, gsplat. Don't re-implement
   what they provide.
4. **No fake algorithms, placeholder outputs or fabricated metrics.** If something can't
   run, say so and record why.
5. CPU execution must work for everything except splat training; GPU is optional acceleration.
6. Keep every intermediate result and processing metadata (§4.3).
7. Don't implement features of other parts (capture SDK, live guidance, backend, privacy).
8. Work **one phase at a time**: inspect → define I/O → implement → run a few meaningful
   tests on real/controlled data → **measure** → check integration → report.
9. No piles of mock tests. Each test must check a measured quantity against a threshold.
10. Never claim completion without reproducible evidence (command + output + metrics file).

## 3. Environment setup on the GPU machine

Prereqs: NVIDIA driver, **CUDA toolkit with `nvcc`** (gsplat compiles its CUDA kernels on
first use, and the toolkit major version must match torch's CUDA build), uv, Docker, git, 7z,
≥ 40 GB disk.

```sh
cd 2D-3D/reconstruction
nvidia-smi                         # driver CUDA version
nvcc --version                     # toolkit version
# Pick ONE torch flavour: cu126 (driver >= 560, toolkit 12.x)  or  cu130 (driver >= 580, toolkit 13.x)
uv sync --all-packages --extra cu126 --group dev
# --all-packages is required: without it the workspace members are NOT installed.

docker build -t room-openmvs:2.4.0 docker/openmvs          # ~20-40 min; raise --build-arg JOBS=8
docker build -t room-eth3d-eval docker/eth3d-eval
./scripts/fetch_test_data.sh                                # ~0.9 GB download
```

Pinned versions (from `uv.lock`): torch 2.14.1, torchvision 0.29.x, gsplat 1.5.3,
open3d **0.19.0**, pycolmap 4.2.1, opencv-python 5.0.0.93, numpy 2.5.4 (2.4.6 on py3.11),
scipy, av, torchmetrics 1.9.0, trimesh 5.2.0. Python 3.11–3.12 only (open3d wheels).

### Known library facts (found during setup; don't rediscover them)

| Fact | Consequence |
|---|---|
| open3d **0.20.0** `ScalableTSDFVolume` returns **0 vertices** (single-view plane and 8-view sphere tests), while `UniformTSDFVolume` works and **0.19.0 works** (11532 verts, mean z = 2.00001 for a wall at 2 m). The `t.geometry.VoxelBlockGrid` also returned 0 on 0.20. | open3d pinned to 0.19.0. If you upgrade, re-run `tests/env` TSDF test first. |
| nerfstudio 1.1.5 pins `gsplat==1.4.0` and `opencv-python-headless==4.10.0.84` | nerfstudio is **not** in this env. Use gsplat 1.5.3 directly (`gsplat.rasterization`, `gsplat.strategy.DefaultStrategy`). If you want nerfstudio `splatfacto` as a comparison baseline, put it in a separate venv. |
| pycolmap 4.2.1 wheel from PyPI is CPU-only (`pycolmap.has_cuda == False`) | SIFT extraction/matching and BA run on CPU (fine). PatchMatch stereo needs CUDA: either install `pycolmap-cuda12==4.2.1` in place of `pycolmap` (same module name, so they conflict) or use the `colmap/colmap` Docker image. Default dense path is OpenMVS (CPU). |
| `openmvs/openmvs-ubuntu` on Docker Hub is from 2019 | Use our `docker/openmvs` build of v2.4.0. |
| Torch flavours | `uv sync` without `--extra cpu|cu126|cu130` falls back to the PyPI torch wheel. Always pass an extra. |

### Step 0: environment acceptance tests (do these first; record results)

Write these as `reconstruction/tests/env/test_env.py` (pytest) and run them. Each one
measures something:

| ID | Test | Pass criterion |
|---|---|---|
| E1 | Versions: torch, CUDA available, device name, gsplat, open3d, pycolmap | `torch.cuda.is_available()` is True; versions match the pins |
| E2 | Open3D RANSAC plane on 5000 pts at z=0.5 (σ=5 mm) + 500 outliers | normal within 1° of ±Z, \|d\| within 5 mm of 0.5, ≥ 4950 inliers |
| E3 | Open3D **ScalableTSDFVolume**: fuse ray-cast depth of a GT sphere (r=0.5 m, 8 views) | radius mean error < 5 mm, std < 5 mm |
| E4 | gsplat forward: rasterize 1 Gaussian at known position with known intrinsics | rendered peak pixel within 1 px of the analytic projection |
| E5 | gsplat backward/optimisation: fit N=2000 Gaussians to one 128×128 synthetic image for 300 steps | PSNR increases monotonically (smoothed) and final PSNR > 25 dB |
| E6 | pycolmap SfM on 30 ETH3D office images | ≥ 90 % registered, mean reprojection error < 1.5 px |
| E7 | OpenMVS image: `docker run --rm room-openmvs:2.4.0 DensifyPointCloud --help` | exit 0 |
| E8 | ETH3D eval tool: score the GT scan against itself (sanity) | F-score ≈ 1.0 at 2 cm |

## 4. Interfaces (define these first, in Phase 1, before algorithms)

### 4.1 Part 3 input: `CaptureInput` adapter interface

The Scan Bundle spec (Project A) isn't written yet, so Part 3 reads through an adapter
layer, and the Scan Bundle adapter is added later (TODO, depends on Project A spec v0.1):

```
room_spatial.io.CaptureInput  (in-memory, produced by an adapter)
  frames:      iterator of (frame_index, timestamp_ns:int64, image: HxWx3 uint8, path)
  intrinsics:  fx, fy, cx, cy [px] for the stored resolution, width, height, distortion model+coeffs
  poses:       optional T_world_camera (4x4, metres) per frame + tracking_state
  depth:       optional per-frame metric depth (float32 metres) + confidence
  imu:         optional (timestamp_ns, gyro rad/s, accel m/s²)
  gravity:     optional unit vector in world frame
  source:      adapter name + dataset path + checksum
Adapters to write: eth3d (images + COLMAP GT), tum_rgbd (rgb+depth+groundtruth.txt+accelerometer),
                   scanbundle (TODO, after capturekit/spec exists)
```

Canonical conventions (keep these identical to what the CaptureKit spec proposes):
metres, radians, int64 ns timestamps; camera frame OpenCV (x right, y down, z forward);
world right-handed, gravity-aligned, +Z up; poses `T_world_camera`; quaternions `[w,x,y,z]`
Hamilton. ETH3D/COLMAP store `T_camera_world` (world-to-camera) and are **not** gravity-aligned.
TUM stores camera-to-world `tx ty tz qx qy qz qw` (note: xyzw order). Convert explicitly,
and add a round-trip unit test per adapter.

### 4.2 Part 3 output → Part 4 input: `SpatialMap` folder (versioned)

```
<run>/spatial_map/
  manifest.json            format "room.spatial_map", version "0.1.0", inputs, params, tool versions, timings, sha256 of every file
  cameras/                 refined intrinsics + T_world_camera per keyframe (JSON) + COLMAP model (cameras.bin images.bin points3D.bin)
  sparse/                  pycolmap reconstruction used for refinement
  depth/                   per-keyframe depth (.npy float32 m) + confidence + source (sensor|mvs)
  fused_cloud.ply          fused point cloud (xyz, rgb, normal), world frame, metres
  tsdf_mesh.ply            TSDF mesh (optional, used for planes/room)
  planes.json              [{id, label floor|ceiling|wall|other, normal, d, inlier_count, area_m2, polygon}]
  room.json                {floor_polygon, ceiling_height_m, wall_ids, bbox, gravity, scale_source, scale_confidence}
  quality.json             measured metrics for this run
```
Write a JSON Schema for `manifest.json`, `planes.json`, `room.json` in
`reconstruction/spec/` and document it in `reconstruction/docs/interfaces/spatial_map.md`.

### 4.3 Part 4 output: `Reconstruction` folder (versioned)

```
<run>/reconstruction/
  manifest.json            format "room.reconstruction", version "0.1.0", input spatial_map sha256, params, versions, timings, hashes
  sparse/                  final SfM model
  dense/                   OpenMVS scene.mvs, dense point cloud .ply
  mesh/                    mesh.ply, mesh_refined.ply (optional), textured/{mesh.obj, mesh.mtl, *.png}, mesh.glb
  splat/                   point_cloud.ply (3DGS standard attributes), cfg.json, training log, checkpoints (last only)
  eval/                    held-out view renders + metrics.json (per output: PSNR/SSIM/LPIPS, F-score)
```
Mesh and splat each have their own manifest entry and their own metrics; neither is built
from the other.

### 4.4 Run directories and metadata

Every command writes to `reconstruction/runs/<YYYYmmdd-HHMMSS>_<phase>_<dataset>/` (gitignored):
`cmd.txt` (exact command), `env.json` (versions, GPU, git commit), `params.json`,
`stage_*/` intermediates, `metrics.json`, `log.txt`. Reports quote numbers from these files.

## 5. Phases, tests and acceptance criteria

Thresholds are **PROPOSED** defaults. Tighten or relax them only with a written reason in
the phase report, and never just to make a test pass.

Datasets (all fetched by `scripts/fetch_test_data.sh`):
- **ETH3D office** (indoor room, 26 DSLR images, undistorted, GT poses + GT laser scan; CC BY-NC-SA 4.0).
- **TUM RGB-D fr1/xyz** (RGB + Kinect depth + GT trajectory + accelerometer; CC BY 4.0).
- **Synthetic room** (you generate it: an Open3D box room 4×3.5×2.6 m with a table and
  textured walls, ray-cast RGB-D from a known trajectory). It's the only source of exact
  ground truth for planes/room dimensions. Keep the generator deterministic (seeded).

| Phase | Module | Work | Tests (measured) | Accept (PROPOSED) |
|---|---|---|---|---|
| P1 | spatial | `CaptureInput` + adapters (eth3d, tum, synthetic) + keyframe selection (sharpness via Laplacian variance, baseline/rotation thresholds) | Round-trip pose conversion per adapter; TUM depth/RGB association max Δt; keyframe count vs frames | Round-trip error < 1e-9; association Δt ≤ 20 ms; keyframes cover trajectory (no gap > 0.3 m or 15°) |
| P2 | spatial | Pose refinement: pycolmap features+matching on keyframes, triangulate with input poses as priors, bundle adjustment; SfM fallback when no poses | (a) TUM fr1/xyz: perturb GT poses (σ=3 cm, 2°), refine, ATE/RPE vs GT. (b) ETH3D: SfM-only, Sim(3)-align to GT | (a) ATE RMSE reduced ≥ 50 % vs perturbed and < 2 cm; (b) ≥ 95 % registered, median position error < 2 cm, reproj < 1.0 px |
| P3 | spatial | Depth + scale: use sensor depth when present; otherwise MVS depth (OpenMVS DensifyPointCloud depth maps, or COLMAP PatchMatch on GPU); scale from metric input poses, or from sensor depth ratio | TUM: MVS depth vs Kinect depth on valid pixels; scale recovery on a deliberately scaled (×0.37) SfM model | AbsRel ≤ 0.10, δ<1.25 ≥ 0.90; scale error ≤ 2 % |
| P4 | spatial | Fusion: Open3D ScalableTSDF (sensor/MVS depth) → mesh + cloud; statistical outlier removal; voxel downsample | ETH3D office: fused cloud vs GT scan via **ETH3D eval tool**; synthetic room: Chamfer vs GT mesh | ETH3D F-score@5 cm ≥ 0.60 (report 1/2/5/10 cm); synthetic Chamfer-L1 ≤ 1 cm |
| P5 | spatial | Planes + room: iterative RANSAC (Open3D `segment_plane`) + region merging; label by gravity (floor = lowest horizontal, ceiling = highest); walls = vertical planes; floor polygon from wall intersections / alpha shape of floor inliers; ceiling height | Synthetic room (exact GT); ETH3D office: floor/wall normal orthogonality | Synthetic: plane normal error ≤ 2°, offset ≤ 2 cm, ceiling height error ≤ 2 cm, floor area error ≤ 3 %; ETH3D: floor⟂wall within 3° |
| P6 | spatial | `SpatialMap` writer + schemas + `room-spatial run` CLI (typer) | Schema validation; re-running with same seed gives identical hashes for deterministic files; Part 4 reader loads it | Schema-valid; deterministic; loader passes |
| P7 | recon | Sparse: reuse SpatialMap COLMAP model, or re-run SfM if absent; export for OpenMVS (`InterfaceCOLMAP`) and gsplat | ETH3D office | ≥ 95 % registered; reproj < 1.0 px; files load in OpenMVS |
| P8 | recon | Dense + mesh + texture via OpenMVS docker: DensifyPointCloud → ReconstructMesh → (RefineMesh) → TextureMesh; CPU fallback Open3D Poisson; export glb (trimesh) | ETH3D eval tool on dense cloud and on mesh-sampled points; texture: render textured mesh at held-out GT views (Open3D/pyrender offscreen) | Dense F-score@5 cm ≥ 0.70; mesh F-score@5 cm ≥ 0.65; textured held-out PSNR ≥ 18 dB |
| P9 | recon | Gaussian Splatting with gsplat 1.5.3 (own trainer around `gsplat.rasterization` + `DefaultStrategy`; init from sparse points; held-out every 8th image); export standard 3DGS `.ply` | ETH3D office at ½ res; TUM fr1/xyz; record train time, VRAM peak, #Gaussians | Held-out PSNR ≥ 24 dB, SSIM ≥ 0.80, LPIPS ≤ 0.30 on ETH3D (report all); PLY loads in a 3DGS viewer and gsplat re-render of it matches training render (PSNR > 40 dB) |
| P10 | recon | `Reconstruction` packaging + schemas + `room-recon run` CLI; end-to-end `room-spatial run` → `room-recon run` on ETH3D and synthetic | Schema validation; manifest hashes verify; end-to-end wall time and peak RAM/VRAM | All schema-valid; hashes verify; E2E completes from one command each |

CPU-vs-GPU: P1–P8 must also complete on CPU (`--device cpu`, maybe at reduced resolution).
Record both timings where a GPU path exists.

## 6. Phase report format (post after every phase, then stop for review)

```
Phase Pn: <name>
Files changed: <list>
Commands run: <exact commands>
Evidence: runs/<dir>/metrics.json   (paste the key numbers, not the whole file)
Result vs acceptance: <metric = value (threshold) PASS/FAIL> ...
Integration check: <what adjacent module consumed the output, and how it was verified>
Limitations / not verified: <...>
```

## 7. Open questions for the project owner

- Are Parts 3/4 the same as "Project C (reconstruction)" in the CaptureKit brief? Interface docs assume yes.
- ETH3D is CC BY-NC-SA: OK for internal evaluation only? (Assumed yes; never redistribute.)
- Acceptance thresholds in §5 are proposals; confirm them after P2 shows realistic numbers.
- Scan Bundle adapter waits on the CaptureKit spec (coordinates.md + schemas), which hasn't been written yet.
