# Interfaces: roomscan-recon (Part 3 `spatial/`, Part 4 `recon/`)

Single source of truth for on-disk formats between phases. Pydantic models live in
`formats/`; every JSON file below validates against them. Version bumps follow semver
(patch = docs, minor = additive optional field, major = breaking).

## 0. Conventions (all phases)

| Item | Convention |
|---|---|
| Units | metres, radians; depth PNGs are uint16 **millimetres** (0 = invalid) |
| Time | int64 nanoseconds (`timestamp_ns`) |
| Camera frame | OpenCV: x right, y down, z forward |
| World frame | right-handed, **gravity-aligned, +Z up** (after Phase 2), metres |
| Pose | `T_world_camera` (camera→world), 4×4 row-major in JSON; COLMAP files store the inverse (`cam_from_world`), as COLMAP does |
| Intrinsics | pinhole `fx, fy, cx, cy` in pixels **for the stored image size**; images in a workspace are undistorted |
| Frame id | zero-padded string, `"000123"`, identical across `images/`, `depth/`, JSON keys |
| Ground truth | lives in `workspace/gt/` and is read **only** by `eval/`. Pipeline code must never read it |

## 1. Phase 1: ingest → workspace

`python -m spatial.cli ingest --dataset {replica,tum,eth3d} --src <path> --out runs/<run_id> [--max-frames N] [--stride K] [--downscale D | --max-side N] [--prior {gt,gt_noisy,none}] [--noise-preset {vio_small,vio_large}] [--seed S]`

`--downscale D` is an integer INTER_AREA reduction. `--max-side N` (opt-in, excludes `--downscale > 1`) INTER_AREA-resizes to a long side of N for non-integer ratios (never upscales; size `round(W*s) x round(H*s)`, `s = N / max(W, H)`). Both use the pixel-centre rule for K: `fx, fy *= s`, `c' = (c + 0.5) * s - 0.5`. Measured effect vs COLMAP's internal resize: PROGRESS.md (T-027).

```
runs/<run_id>/workspace/
  images/<frame>.jpg            undistorted RGB (quality 95) at stored resolution
  depth/<frame>.png             optional sensor depth, uint16 mm, registered to images
  depth_conf/<frame>.png        optional uint8 confidence (0 low … 255 high)
  priors.json                   formats.Priors (below)
  workspace_meta.json           formats.WorkspaceMeta
  gt/poses_gt.json              eval only: {frame: T_world_camera} in the dataset GT frame
  gt/gt_ref.json                eval only: path + sha256 of GT mesh/cloud, GT frame notes
runs/<run_id>/phase1/meta.json  formats.StageMeta
```

`priors.json` (`formats.Priors`, v0.1.0)

| Field | Type / unit | Meaning |
|---|---|---|
| `version` | semver | format version |
| `width`, `height` | px | stored image size |
| `intrinsics` | `{fx,fy,cx,cy}` px | shared pinhole intrinsics (after undistortion + downscale) |
| `frames[]` | list | per frame: `id`, `timestamp_ns`, `image`, `depth` (nullable), `depth_conf` (nullable) |
| `frames[].T_world_camera` | 4×4 or null | **prior** pose (VIO-like); null when `pose_source = none` |
| `pose_source` | enum `arkit, arcore, dataset_gt, dataset_gt_noisy, none` | where priors come from. `dataset_gt_noisy` = GT + simulated VIO drift (benchmark only) |
| `pose_noise` | object or null | noise preset + seed when `dataset_gt_noisy` |
| `gravity_world` | unit 3-vector or null | gravity direction (pointing **down**) in the prior world frame |
| `gravity_source` | enum `arkit, imu_accel, dataset_axis, none` | how gravity was obtained |
| `depth_scale_m` | float | metres per depth unit (0.001) |
| `depth_source` | enum `lidar, kinect, rendered, none` | sensor type of `depth/` |

## 2. Phase 2: pose refinement → refined workspace

`python -m spatial.cli refine --run runs/<run_id> [--max-image-size 1024] [--matcher {exhaustive,sequential}] [--device cpu|cuda]`

Adds to the workspace (inputs above are unchanged):
```
workspace/sparse/0/{cameras.bin, images.bin, points3D.bin}   metric, gravity-aligned (+Z up), our world frame
workspace/sparse/raw/                                        unaligned SfM model (kept as intermediate)
workspace/poses_refined.json                                 {frame: T_world_camera} (same frame as sparse/0)
workspace/alignment.json                                     Sim(3) raw→prior, gravity rotation, residuals
runs/<run_id>/phase2/meta.json
```
Rules: scale comes from the metric priors (Sim(3) RANSAC over camera centres); gravity
alignment rotates `gravity_world` to −Z. Frames not registered by SfM are listed in
`alignment.json.unregistered` and are absent from `sparse/0` and `poses_refined.json`.

## 3. Phase 3: depth + fusion

`python -m spatial.cli phase3 --workspace runs/<run_id>/workspace --depth-source {sensor,mono,auto} --model {small,base,large} --voxel 0.02 --device {cpu,cuda} --downscale N --max-frames N [--no-align] [--no-masks] [--no-consistency] [--pose-source {refined,gt}]`

Input: the Phase 2 workspace (`images/`, optional `depth/`, `depth_conf/`, `sparse/0/`, `priors.json`, `workspace_meta.json`). `--pose-source gt` exists only for the R0 oracle.

```
runs/<run_id>/phase3/
  depth_raw/<frame>.npy         cached model prediction (float32 m or relative), keyed by input+model hash
  depth_aligned/<frame>.png     uint16 mm, metric
  depth_valid/<frame>.png       uint8 {0,255}, mask after all enabled filters
  depth_alignment.json          formats.DepthAlignment
  fusion/cloud.ply              fused cloud: xyz (m, world), rgb, normals
  fusion/tsdf_mesh.ply          TSDF mesh, debugging/eval only (NOT a Part 4 output)
  meta.json                     formats.StageMeta
```

`depth_alignment.json` (`formats.DepthAlignment`)

| Field | Meaning |
|---|---|
| `model` | `{name, hf_id, revision, kind: metric|relative|inverse_relative, licence}` or `sensor` |
| `global_scale` | float or null (when global-scale option used) |
| `frames.<id>.source` | `sensor`, `mono` |
| `frames.<id>.scale`, `.shift` | depth = scale·pred + shift (shift 0 for metric models) |
| `frames.<id>.scale_smoothed` | after temporal median filter |
| `frames.<id>.inliers`, `.n_points` | RANSAC inliers / projected sparse points |
| `frames.<id>.residual_m` | median abs residual of inliers (m) |
| `frames.<id>.status` | `ok`, `rejected_few_points`, `rejected_residual`, `no_alignment` |

## 4. Phase 5 (HANDOFF P5): planes + room shell

`python -m spatial.cli room --cloud <run>/phase3/fusion/cloud.ply --out <run>/phase5 [--workspace <ws>] [--voxel 0.03] [--seed 0]`

Input: a fused cloud in the gravity-aligned world frame (+Z up, metres). With `--workspace`, the
command refuses a workspace whose `alignment.json` says `gravity_aligned: false`, and records the
scale source from `priors.json`. Code: `spatial/room/` (planes.py algorithm, run.py stage).

```
<run>/phase5/
  planes.json     formats.PlanesFile: planes[{id, label floor|ceiling|wall|other, normal (unit,
                  pointing into the room), d (n·x + d = 0), inlier_count, area_m2 (convex hull of
                  projected inliers), rms_m, polygon (hull, 3D)}], n_points, n_assigned, params
  room.json       formats.RoomModel: floor_polygon (2D xy, CCW), floor_polygon_source
                  (wall_intersections | floor_hull | none), floor_area_m2, floor_z/ceiling_z (median
                  inlier z), ceiling_height_m (perpendicular floor→ceiling distance), floor_id,
                  ceiling_id, wall_ids, bbox_min/max, gravity_world, scale_source,
                  scale_confidence (null: no estimator yet), notes[]
  meta.json       formats.StageMeta (metrics include floor_tilt_deg = residual frame tilt)
```

Rules: floor = lowest large horizontal plane facing up; ceiling = highest facing down; wall =
vertical plane with area ≥ 0.8 m², vertical span ≥ 50 % of room height, reaching within 0.35 m of
the ceiling (rejects cabinets and tables). Floor polygon = intersections of consecutive
*dominant* walls (largest wall per direction within 20°), validated against the floor hull,
kept when the top-down footprint contour adds no blob wider than 0.20 m. Otherwise (recess or
bay, or invalid wall polygon) the footprint contour is used, with edges snapped to the detected
wall lines (`floor_polygon_source` stays `footprint_contour`); fallback is the floor hull.

## 5. `meta.json` for every stage (`formats.StageMeta`)

`stage, status (ok|failed|partial), started/ended (ISO 8601), wall_time_s, device, peak_ram_mb,
peak_vram_mb|null, versions{python, numpy, open3d, pycolmap, torch, transformers, git_commit},
params{…}, inputs{path: sha256}, outputs{path: sha256}, metrics{…}, notes[]`

## 6. Phase 6: SpatialMap (Part 3 output, Part 4 input)

`python -m spatial.cli run --dataset {replica,tum,eth3d} --src <scene> --out <run> [--max-frames N] [--stride K] [--downscale D] [--prior gt|gt_noisy|none] [--depth-scale] [--phase3-downscale 2]`
runs P1 → P2 → P3 → P5 → P6 and writes `<run>/spatial_map/`. Code: `spatial/pipeline.py`,
`spatial/spatial_map.py`; reader/verifier (no `spatial` import): `formats/spatial_map.py`.
Large files are hard-linked from the stage outputs when possible. Schemas: `spec/spatial_map/*.json`
(generated from `formats/models.py` by `scripts/export_schemas.py`; a test fails if stale).

```
spatial_map/
  manifest.json      SpatialMapManifest: format "room.spatial_map", version, created, source, frame_ids,
                     files{path: sha256} of EVERY other file, stages{phase2|3|5: status, wall_time_s,
                     peak_ram_mb, device}, params, versions, conventions
  cameras/cameras.json  CamerasFile: image + depth intrinsics, pose_source, metric, gravity_aligned,
                     frames{id: T_world_camera} for all registered frames
  images/<id>.jpg    undistorted images of all registered frames (needed by dense, texture, splats)
  sparse/            COLMAP cameras.bin images.bin points3D.bin (cam_from_world, as COLMAP)
  depth/<id>.png     uint16 mm metric depth of the fused frames; depth/depth_alignment.json
  depth_valid/<id>.png  final validity mask (255)
  fused_cloud.ply    fused point cloud (xyz, rgb, normals), world frame
  tsdf_mesh.ply      TSDF mesh (debug / planes only: NOT a Part 4 output)
  planes.json room.json   §4
  quality.json       SpatialQuality: registration, scale source/confidence, depth validity,
                     wall/floor deviation, floor tilt, and `warnings[]` (rule-based, see
                     spatial/spatial_map.py quality_report)
```
`load_spatial_map(path)` re-hashes every listed file and reports missing, modified and
unlisted files (`verify()`), so Part 4 never consumes a corrupted or half-written map.
Version policy: 0.x; additive optional fields bump the minor version.

Depth sources (Phase 3, `--depth-source`): `sensor`; `mvs` (OpenMVS depth maps, confidence ≥ 0.5;
default when no sensor); `hybrid` (MVS, holes filled with scale-aligned mono depth: measured no
better than `mvs`, kept as an option); `mono` (Depth-Anything-V2-Small aligned to sparse points:
fallback only). `refine --depth-scale` corrects the metric scale from sensor depth.

## 7. Phase 10: Reconstruction (Part 4 output)

`python -m recon.cli run --spatial-map <run>/spatial_map --out <run>/reconstruction [--resolution-level 1] [--splat auto|yes|no] [--heldout-every 8] [--refine]`
Code: `recon/` (sparse.py, dense.py, openmvs.py, render.py, evaluate.py, splat/, package.py);
reader/verifier: `formats/reconstruction.py`. `recon` never imports `spatial`.

```
reconstruction/
  manifest.json   ReconstructionManifest: format "room.reconstruction", spatial_map{path,
                  manifest_sha256, quality_warnings}, outputs{sparse|dense|mesh|splat: OutputEntry},
                  files{path: sha256}, timings_s, peak_ram_mb, params, versions
  sparse/         the model Part 3 refined, unchanged
  dense/          OpenMVS scene_dense.mvs/.ply
  mesh/           mesh.ply, [mesh_refined.ply], textured/mesh.obj|mtl|textures, mesh.glb
  splat/          point_cloud.ply (standard 3DGS layout), cfg.json, train_log.jsonl, ckpt_last.pt
  eval/           metrics.json, mesh_heldout/<frame>_render_vs_gt.jpg, splat metrics
```
`OutputEntry = {status: ok|skipped|failed, reason, files[], metrics{}}`. Mesh and splat are
separate entries with separate metrics; the splat is trained from the SpatialMap, never from
the mesh. A product that cannot be made is `skipped`/`failed` with the reason (e.g. splat on a
machine without CUDA); nothing is fabricated. Held-out: every `--heldout-every`-th image is
removed from the sparse model before densification/texturing and from splat training, and the
held-out photographs are the evaluation target.

## 8. Exports, QA report, router (consumers of the SpatialMap / Reconstruction)

All three read only `formats/` (enforced by `tests/test_module_boundaries.py`).

- `python -m export.cli all --spatial-map <dir> --out <dir>`: `floorplan.svg`, `floorplan.dxf`
  (R2010, metres; layers FLOOR_OUTLINE, WALLS, DIMENSIONS, NOTES), `room_shell.usda` (closed,
  consistently oriented room shell; `usdzip` converts to USDZ), `fused_cloud.laz` (1 mm),
  `measurements.json` (area, perimeter, wall lengths, ceiling height, volume, scale confidence).
  A room without a floor polygon is reported under `skipped`, never written empty. E57 is not
  produced (convert the LAZ with PDAL/CloudCompare); splat compression (SPZ) and glTF
  `KHR_gaussian_splatting` are not implemented.
- `python -m qa.cli report --spatial-map <dir> [--reconstruction <dir>] --out job_report.json`:
  `JobReport` (`spec/qa/job_report.schema.json`): `verdict` pass | pass_with_warnings |
  needs_rescan, `gates[]` (category capture|output, status pass|warn|fail|skipped, value,
  threshold), per-surface `coverage`, and `rescan[]` regions with 4 world-frame corners, reason
  (`unseen`, `few_views`, `narrow_baseline`), priority and a readable instruction. Thresholds:
  `qa/thresholds.py`. Coverage ignores occlusion. A failed *capture* gate gives needs_rescan; a
  failed *output* gate (mesh/splat quality) never does, but is never hidden; a skipped splat is
  not a pass.
- `python -m router.cli plan -o mesh -o floorplan --tier A [--cuda] [--budget-min N]`: stage plan
  (commands, where=local|gpu_worker, estimates), refusals (RGB-only semantic outputs, scans that
  failed QA) and warnings. Never lowers accuracy settings to meet a budget (`over_budget` flag).

## 9. Module boundaries

- `spatial/` (Part 3) and `recon/` (Part 4) never import each other.
- Both may import `formats/`. Only tests/benchmarks import `eval/`; pipeline code never does.
- `eval/` may read `workspace/gt/`; nothing else may.
