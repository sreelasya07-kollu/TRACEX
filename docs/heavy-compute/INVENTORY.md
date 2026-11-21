# Heavy-compute inventory (generated from inventory.yaml: do not edit by hand)

All resource numbers are **ESTIMATES** unless the basis says MEASURED. Calibration runs
(measured locally) are listed at the end. `status: not_implemented` means the code does
not exist yet. That is a missing feature, not a hardware limit.

## Summary

| Quantity | Value |
|---|---|
| Items | 37 (implemented: 11, not_implemented: 18, partial: 8) |
| Priority | P0: 4, P1: 20, P2: 13 |
| Requirement classes (items may have several) | GPU: 22, HIGH_RAM: 11, LARGE_DISK: 10, LONG_CPU: 13 |
| GPU-hours (sum over GPU items with a parsable runtime) | ESTIMATE 19-49 h; not summable: P3-03, P3-05, P6-01, P7-04, P9-01, P9-02, P9-03 |
| Peak VRAM (largest single item) | ESTIMATE 24 GB |
| Peak system RAM (largest single item) | ESTIMATE 64 GB |
| Dataset downloads (all used datasets, MEASURED HEAD sizes) | 58.7 GB (excl. real Scan Bundles: none exist) |
| Largest single-item working disk | ESTIMATE 100 GB |

## Datasets

| key | name | download | licence | note |
|---|---|---|---|---|
| replica_full | Replica (NICE-SLAM render), 8 scenes, all 2000 frames | 12.44 GB | Replica research licence (non-commercial) | room0 1.81 GB, office0 1.44 GB compressed; per-scene extraction via range reads (data/fetch_replica.py --stride 1) |
| replica_cull | Replica culled GT meshes | 0.151 GB | as Replica |  |
| arkitscenes_2 | ARKitScenes raw, Validation 42444946 (visit 421337) + 42444966 (visit 421383): lowres_wide, lowres_depth, confidence, intrinsics, traj, 3dod mesh + laser scans | 23.64 GB | ARKitScenes licence (Apple, non-commercial research) | per-file HEAD via scripts/heavy/fetch_datasets.sh: capture assets 1.31 GB, 3dod meshes 0.07 GB, laser scans 14.91 GB (visit 421337, 8 x ~1.8-1.9 GB) + 7.36 GB (visit 421383, 4 scans) |
| tum_fr1 | TUM RGB-D fr1/desk + fr1/xyz | 0.79 GB | CC BY 4.0 |  |
| nyu_test | NYU Depth v2 labelled (654 test split) | 2.97 GB | NYU research use |  |
| eth3d_office | ETH3D office DSLR undistorted + scan eval | 0.44 GB | CC BY-NC-SA 4.0 |  |
| eth3d_train_all | ETH3D high-res multi-view training, all 13 scenes (undistorted) | 5.89 GB | CC BY-NC-SA 4.0 |  |
| mipnerf360 | Mip-NeRF 360 (all 9 scenes; one used) | 12.54 GB | see dataset page (research) |  |
| scanbundles_real | Real CaptureKit Scan Bundles (ARKit + ARCore, same room) | n/a | internal (consented captures) | DO NOT EXIST YET: CaptureKit spec and SDK not implemented |

## Phase 0

| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P0-01 | Build CUDA Docker image (COLMAP-CUDA incl. global mapper, pycolmap-cuda12, Open3D-CUDA, PyTorch-CUDA, gsplat, opt. nerfstudio) | partial | docker/cuda.Dockerfile written 2026-11-13 but UNBUILT/UNVERIFIED: no NVIDIA GPU, 9.7 GB free disk. OpenMVS stays in docker/openmvs (needs Ubuntu 26.04 OpenCV). | GPU/LARGE_DISK | 0 / 16 / 40 | 1-3 h build | ASSUMPTION: CUDA devel base images are ~5-8 GB, COLMAP+OpenMVS+Open3D source builds add tens of GB of build cache; scale from C8 (CPU OpenMVS build alone = 0.6 GB image) | - | `docker build -f docker/cuda.Dockerfile -t roomscan-cuda:dev .` | roomscan-cuda:dev image | build_time_s, image_size_gb → image builds; `nvidia-smi`, `colmap -h`, `glomap -h`, `DensifyPointCloud --help`, torch.cuda.is_available(), o3d.core.cuda.is_available() all succeed inside | P0 | - |
| P0-02 | Environment GPU acceptance tests E4 (gsplat projection) + E5 (gsplat fit, PSNR > 25 dB) | implemented | tests/test_env.py:16 skipif(not torch.cuda.is_available()) -> SKIPPED here | GPU | 1 / 4 / 0 | < 2 min incl. first JIT compile of gsplat kernels | ASSUMPTION: 2000 Gaussians, 128x128, 300 steps; JIT compile dominates | - | `uv sync --extra cu126 && .venv/bin/python -m pytest -m gpu tests/test_env.py -k 'e4 or e5'` | pytest report | psnr_start, psnr_end → E4 peak within 1 px of analytic projection; E5 final PSNR > 25 dB (HANDOFF.md §3) | P0 | P0-01 |

## Phase 1

| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P1-01 | Ingest round-trip on full-length real Scan Bundles (10-min captures) | not_implemented | Scan Bundle adapter not written: CaptureKit spec (capturekit/spec/scanbundle) has no schemas yet; no real bundles exist | LARGE_DISK | 0 / 4 / 20 | TARGET TBD | ASSUMPTION: 10 min @30 fps = 18k frames; JPEG ~0.15 MB/frame at 1920x1440 -> ~3 GB/bundle images + depth; no measurement possible | scanbundles_real | `python -m spatial.cli ingest --dataset scanbundle --src <bundle> --out runs/heavy/ingest_<id>   # adapter TODO` | runs/heavy/ingest_<id>/workspace | wall_s, disk_gb, frames → TARGET TBD | P1 | - |
| P1-02 | Ingest all Replica frames (stride 1, full 1200x680) for room0/office0 | implemented | downsized: data fetched at stride 5 (data/fetch.sh REPLICA_STRIDE=5) and ingested at stride 4 + downscale 2 (benchmarks/phase3.py:30-32) due to disk (9.7 GB free) and RAM | LARGE_DISK | 0 / 2 / 8 | ~10 min/scene | MEASURED compressed size room0 1.81 GB + office0 1.44 GB (HEAD); workspace JPEG re-encode ~ same size; time ASSUMPTION I/O bound | replica_full | `REPLICA_STRIDE=1 data/fetch.sh replica && python -m spatial.cli ingest --dataset replica --src data/replica/room0 --stride 1 --prior gt_noisy --out runs/heavy/P1-02_room0` | runs/heavy/P1-02_room0/workspace | wall_s, disk_gb, n_frames → 2000 frames ingested; Phase 1 convention tests still pass | P1 | - |

## Phase 2

| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P2-01 | COLMAP GPU SIFT extraction + matching, all keyframes, full resolution | partial | pycolmap wheel is CPU-only (has_cuda False); runs use max_image_size 640-1600 (tests/test_phase2.py, benchmarks/phase3.py:88) instead of full res | GPU | 4 / 16 / 5 | 0.5-2 h per 2000-frame scene | scaled from C4 (60 frames @640 in 47.6 s CPU); sequential matching is linear in N, extraction ~ pixels; GPU speedup factor is an ASSUMPTION (5-20x) | replica_full, arkitscenes_2 | `pip install pycolmap-cuda12==4.2.1 (replaces pycolmap) && python -m spatial.cli refine --run runs/heavy/P1-02_room0 --max-image-size 0` | runs/heavy/P1-02_room0/workspace/sparse/0, phase2/meta.json | registered, mean_reproj_px, wall_s, peak_vram_gb → registered >= 95 %, reproj < 1.0 px (HANDOFF.md §5 P2) | P1 | P0-01, P1-02 |
| P2-02 | Sequential vs exhaustive vs vocab-tree matching on 500-2000 images | partial | sequential+spatial and exhaustive implemented (spatial/sfm/refine.py); vocab-tree not wired; exhaustive on 2000 images = 2M pairs: never attempted (LONG_CPU) | GPU/LONG_CPU | 4 / 32 / 10 | exhaustive 2000 imgs: 4-12 h (ESTIMATE); seq: < 1 h | pairs scale N^2/2; C5 = 26 imgs exhaustive (325 pairs) 17.6 s CPU -> 2M pairs ~ 30 h CPU, GPU matching ASSUMPTION 5-10x faster | replica_full | `for m in seq exhaustive vocab; do python -m spatial.cli refine --run runs/heavy/P2-02_$m --matcher $m; done   # vocab: TODO` | runs/heavy/P2-02_*/phase2/meta.json | registered, reproj_px, ate_se3_m, wall_s → TARGET TBD (comparison) | P2 | P2-01 |
| P2-03 | hloc SuperPoint + LightGlue (GPU) vs SIFT | not_implemented | hloc not a dependency; no code path | GPU | 8 / 16 / 10 | 1-3 h | ASSUMPTION (not measured; no local implementation) | replica_full, arkitscenes_2, eth3d_train_all | `TODO: implement spatial/sfm/hloc_features.py` | - | registered, reproj_px, ate, wall_s → TARGET TBD | P2 | P0-01 |
| P2-04 | Global SfM (GLOMAP, now COLMAP 4.x 'global' mapper) vs COLMAP incremental | not_implemented | COORDINATE CHANGE: github.com/colmap/glomap is archived; GLOMAP is merged into COLMAP as the global mapper (pycolmap 4.2 GlobalMapperOptions). Not wrapped in spatial/sfm/refine.py | LONG_CPU | 0 / 16 / 5 | TARGET TBD | ASSUMPTION | replica_full, eth3d_train_all | `TODO` | - | registered, ate, wall_s → TARGET TBD | P2 | P2-01 |
| P2-05 | Full-dataset ATE/RPE before vs after refinement (all Replica scenes, ETH3D 13 scenes, TUM fr1/*) | implemented | downsized: only room0 (80 frames), TUM fr1/xyz (60 frames), ETH3D office (26) run; tests/test_phase2.py max_frames 60-80 | LONG_CPU/LARGE_DISK | 0 / 16 / 25 | 4-10 h CPU total | scaled from C4/C6 (~0.2-0.8 s per frame CPU) x ~16k Replica frames + ETH3D 13 scenes | replica_full, eth3d_train_all, tum_fr1 | `for sc in room0 office0; do python -m spatial.cli ingest --dataset replica --src data/replica/$sc --stride 1 --prior gt_noisy --out runs/heavy/P2-05_$sc && python -m spatial.cli refine --run runs/heavy/P2-05_$sc; done   # + ETH3D 13 scenes: adapter handles one scene dir each` | runs/heavy/<date>/P2-05/ | ate_none, ate_se3, ate_sim3, rpe, scale_error, registered → per HANDOFF.md §5 P2: ATE_se3 reduced >= 50 % and < 2 cm; registered >= 95 % | P1 | P1-02 |
| P2-06 | Sim(3) scale-recovery accuracy across all scenes | implemented | measured only on 3 sequences (scale err 0.10-2.35 %) | LONG_CPU | 0 / 16 / 0 | included in P2-05 | same runs as P2-05 | replica_full, tum_fr1 | `included in P2-05` | runs/heavy/<date>/P2-05/ | scale_error → scale error < 2 % (tests/test_phase2.py) | P1 | P2-05 |

## Phase 3

| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P3-01 | Mono metric depth (DAv2-S, integrated) on ALL frames of each eval scene, CPU vs GPU fp16 | implemented | CPU only: 1.6 s/frame (C2); 100 frames/scene used; no CUDA for fp16 path (spatial/depth/sources.py:61) | GPU/LONG_CPU | 2 / 8 / 12 | GPU: < 1 h for 16k frames (ASSUMPTION 20-50x vs C2); CPU fallback: ~7 h for 16k frames (C2 x 16k) | C2 measured; cache 1200x680 float32 = 3.3 MB/frame -> 16k frames = 52 GB at full res, 13 GB at /2 | replica_full, arkitscenes_2 | `python -m benchmarks.phase3 run --scene room0_full --runs R3 --override '{"device":"cuda","downscale":1}'` | runs/_cache/mono/*.npy, runs/bench_phase3/room0_full/R3/eval.json | ms_per_frame_gpu, ms_per_frame_cpu, peak_vram_gb → TARGET TBD (efficiency, brief Step E) | P1 | P0-01, P1-02 |
| P3-02 | Depth-Anything-V2 Base/Large metric (CC-BY-NC, opt-in) accuracy + speed | implemented | never attempted: licence opt-in + CPU time (Large ~335M params vs 25M; ASSUMPTION ~10x slower than C2) | GPU | 6 / 8 / 2 | 1-3 h | ASSUMPTION from parameter ratio 25M:98M:335M (docs/MODELS.md) | replica_full, arkitscenes_2, nyu_test | `python -m benchmarks.phase3 run --scene room0 --runs R5 --override '{"model":"large","device":"cuda"}' --tag _large` | runs/bench_phase3/room0/R5_large/eval.json | abs_rel, d1, fscore_5cm, ms_per_frame → TARGET TBD; licence: research only | P2 | P0-01 |
| P3-03 | UniDepth / Depth Pro mono depth comparison | not_implemented | not integrated (only DAv2 in spatial/depth/sources.py); licences not reviewed | GPU | 8 / 16 / 5 | TARGET TBD | ASSUMPTION | nyu_test, arkitscenes_2 | `TODO` | - | abs_rel, d1 → TARGET TBD | P2 | P0-01 |
| P3-04 | Step A NYU reproduction (654 images) vs published DAv2 Table 2 (ViT-S rel: AbsRel 0.053, d1 0.973) | implemented | runs on CPU locally (benchmarks/nyu.py), but the first all-at-once attempt died at ~420/654 predictions (no kill record in journal; ~3.5 GB RAM available, 13 GB swap in use). Rewritten to stream in chunks of 16 and resume from cache | LONG_CPU | 2 / 4 / 1.1 | CPU ~20 min (C2 x 654); GPU minutes | C2 scaled to 654 images | nyu_test | `python -m benchmarks.nyu --model relative --device cuda` | runs/test_results/phase3_nyu_relative.json | abs_rel, d1, rel_diff → within ~5 % relative of published AbsRel and d1 (Phase 3 brief §7) | P0 | - |
| P3-05 | Depth accuracy vs GT before/after sparse alignment, all frames, per model | implemented | downsized: 100 frames/scene at 300x170 (benchmarks/phase3.py) | GPU/LONG_CPU | 2 / 16 / 15 | same runs as P3-01 | eval adds ~10 % to C7 | replica_full, arkitscenes_2, tum_fr1 | `python -m benchmarks.phase3 run --scene <scene>_full --runs R2 R3 R4 R5` | runs/bench_phase3/<scene>_full/R*/eval.json | abs_rel, rmse, d1, scale_cv_raw, scale_cv_smoothed → aligned mono vs sensor d1 >= 0.85 (Phase 3 brief §7) | P1 | P3-01 |
| P3-06 | Scale/shift fitting robustness: RANSAC vs least squares, outlier rates, all frames | partial | RANSAC+Huber implemented and light-tested (20 % outliers -> < 1 % scale error); plain-LS baseline flag not implemented | LONG_CPU | 0 / 8 / 0 | < 1 h | alignment ~ms/frame (measured in meta timings align_ms_per_frame) | replica_full, arkitscenes_2 | `TODO: add AlignParams.method=lstsq, then benchmarks.phase3 --override` | - | frames_rejected, inlier_ratio, scale_cv → TARGET TBD | P2 | P3-01 |
| P3-07 | Ablation R0-R6 on ARKitScenes (real LiDAR + ARKit poses) | implemented | ARKitScenes not downloaded (23.64 GB incl. 22.3 GB laser GT > 9.7 GB free); ARKitScenes adapter NOT implemented (spatial/io/adapters.py has replica/tum/eth3d only) | LARGE_DISK/HIGH_RAM | 2 / 32 / 35 | 2-4 h | download MEASURED 23.64 GB; laser-scan GT point clouds 7-13 GB/visit must be loaded/subsampled (ASSUMPTION ≥ 32 GB RAM to load a 13 GB PLY) | arkitscenes_2 | `scripts/heavy/fetch_datasets.sh arkitscenes_2 && python -m benchmarks.phase3 run --scene arkit_42444946 --runs R0 R1 R2 R3 R4 R5 R6   # adapter TODO` | runs/bench_phase3/arkit_*/summary.json | fscore_2cm, fscore_5cm, accuracy, completion, chamfer → R6 F@5cm >= 0.80; R5 beats R2 by >= 0.15 (Phase 3 brief §7) | P1 | P0-01 |
| P3-08 | TSDF voxel sweep 1/2/4 cm, full rooms, all frames: CPU ScalableTSDF vs CUDA VoxelBlockGrid | implemented | CPU legacy+tensor backends verified equal on a sphere (tests/test_phase3_light.py); CUDA:0 device untestable; full-res/all-frames runs exceed local RAM (C1 is 100 frames @300x170) | GPU/HIGH_RAM | 8 / 32 / 5 | < 1 h | C1 scaled: 1 cm voxels -> ~4x surface voxels vs 2 cm; 2000 frames @1200x680 = 20x frames x 16x pixels; RAM ASSUMPTION | replica_full | `for v in 0.01 0.02 0.04; do python -m benchmarks.phase3 run --scene room0_full --runs R0 --override "{\"tsdf_p\":{\"voxel\":$v,\"backend\":\"tensor\",\"device\":\"CUDA:0\"}}" --tag _v$v; done` | runs/bench_phase3/room0_full/R0_v*/eval.json | fscore_5cm, wall_s, peak_ram_gb, peak_vram_gb, n_points → peak RAM <= 8 GB for 300 frames @ 2 cm CPU (Phase 3 brief §7); F-score diff CPU vs GPU < 0.01 (brief §8 gpu) | P1 | P0-01, P1-02 |
| P3-09 | Geometry metrics with >= 1M sampled points per side | implemented | downsized: GT sampled at 400k points (eval/phase3_eval.py n_gt=400_000) to stay within RAM | HIGH_RAM | 0 / 16 / 0 | minutes per run | KD-tree + culling raycast scale ~linearly from C1 eval | replica_full | `python -m benchmarks.phase3 run --scene room0_full --runs R0 R5   # room0_full uses n_gt=1_000_000` | runs/bench_phase3/room0_full/*/eval.json | fscore_2cm, fscore_5cm, chamfer_l1 → \|F(1M) - F(400k)\| reported; TARGET TBD | P1 | P3-08 |
| P3-10 | Large-scene stress: breaking point (max frames, min voxel) | not_implemented | never attempted; no stress driver | HIGH_RAM/GPU | 24 / 64 / 50 | 2-6 h | ASSUMPTION: binary search over frames/voxel until OOM | replica_full, arkitscenes_2 | `TODO: scripts/heavy/stress_phase3.py` | - | max_frames, min_voxel_m, peak_ram_gb → TARGET TBD | P2 | P3-08 |
| P3-11 | Determinism: same input twice -> near-identical outputs | partial | Phase 2 measured non-deterministic (ETH3D ATE 0.91-1.65 cm over 3 runs; 1/9 runs 20/26 registered); Phase 3 repeatability (Step G) not yet run | LONG_CPU | 2 / 16 / 5 | 2x P3-05 | repeat of P3-05 | replica_full | `for k in 1 2 3; do python -m benchmarks.phase3 run --scene room0_full --runs R5 --tag _rep$k; done   # each tag = fresh run; mono predictions shared via cache` | runs/heavy/<date>/P3-11/ | fscore_5cm_mean, fscore_5cm_std → TARGET TBD (report mean ± std) | P1 | P3-05 |
| P3-12 | Phase 3 GPU tests: CUDA fusion + GPU inference parity with CPU | partial | TSDF tensor backend runs on CPU:0 (verified); gpu-marked parity test pending on CUDA | GPU | 4 / 8 / 1 | < 10 min | light test sizes | - | `.venv/bin/python -m pytest -m gpu` | pytest report | fscore_diff → F-score difference < 0.01 (Phase 3 brief §8) | P0 | P0-01 |

## Phase 4

| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P4-01 | Planes + room geometry on 2M / 5M / 20M point clouds vs < 30 s target | not_implemented | Phase 4 not implemented | HIGH_RAM | 0 / 32 / 2 | TARGET < 30 s per cloud (project plan) | ASSUMPTION: 20M points x (xyz+rgb+normal) float32 = 0.7 GB raw; RANSAC + KD-tree overhead | replica_full, arkitscenes_2 | `TODO (Phase 4)` | - | wall_s, peak_ram_gb → < 30 s | P1 | P3-08 |
| P4-02 | Full Phase 4 eval suite on every real scene + tape-measured rooms; parameter-frozen final run | not_implemented | Phase 4 not implemented; tape-measured rooms not recorded; eval/cases.yaml does not exist | LONG_CPU | 0 / 32 / 5 | TARGET TBD | ASSUMPTION | replica_full, arkitscenes_2, scanbundles_real | `TODO` | - | plane_angle_err, room_height_err, floor_area_err → HANDOFF.md §5 P5 (PROPOSED): normal <= 2°, offset <= 2 cm, height <= 2 cm, area <= 3 % | P1 | P4-01 |

## Phase 6

| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P6-01 | Part 4 sparse reconstruction without priors on all eval scenes | not_implemented | recon/ package is empty (only __init__.py) | LONG_CPU/GPU | 4 / 16 / 10 | as P2-05 | same SfM engine as Phase 2 (prior_mode none) | replica_full, eth3d_train_all | `TODO (recon CLI)` | - | registered, reproj_px, ate_sim3 → HANDOFF.md §5 P7: >= 95 % registered, reproj < 1.0 px | P1 | P0-01 |
| P6-02 | Cache/hash behaviour on large runs (skip on re-run, partial re-run after one stage change) | partial | Phase 3 benchmark caches by config hash (benchmarks/phase3.py) and mono predictions by input+model hash; Part 4 pipeline cache not implemented | LARGE_DISK | 0 / 8 / 50 | TARGET TBD | ASSUMPTION | replica_full | `TODO` | - | rerun_wall_s, stages_skipped → unchanged inputs -> all stages skipped | P2 | P6-01 |

## Phase 7

| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P7-01 | COLMAP patch_match_stereo + stereo_fusion (CUDA-only), full res | not_implemented | CUDA-only in COLMAP; no wrapper; pycolmap CPU wheel | GPU/LARGE_DISK | 8 / 32 / 60 | 2-8 h per scene | ASSUMPTION: COLMAP writes per-image depth+normal maps (float32, 2 maps x geometric/photometric) at full res: 1200x680x4 B x 4 ≈ 13 MB/img x 2000 = 26 GB | replica_full, eth3d_office | `TODO` | - | fscore_5cm, wall_s, peak_vram_gb, disk_gb → HANDOFF.md §5 P8: dense F@5cm >= 0.70 | P1 | P6-01, P0-01 |
| P7-02 | OpenMVS DensifyPointCloud (CPU, RAM-heavy) full + half res | partial | Docker image built and binaries verified (E7); no pipeline wrapper; never run on a scene (RAM ~3.5 GB available) | HIGH_RAM/LONG_CPU | 0 / 64 / 30 | 1-6 h per scene | ASSUMPTION; OpenMVS keeps depth maps for all views in memory during fusion | replica_full, eth3d_office | `docker run --rm -v $PWD:/work room-openmvs:2.4.0 DensifyPointCloud scene.mvs --resolution-level 0\|1   # wrapper TODO` | - | fscore_5cm, wall_s, peak_ram_gb → dense F@5cm >= 0.70 (HANDOFF.md §5 P8) | P1 | P6-01 |
| P7-03 | ReconstructMesh -> RefineMesh -> TextureMesh full res; texture atlas size; mesh + texture quality | not_implemented | no wrapper/eval | HIGH_RAM/LONG_CPU | 0 / 64 / 10 | 1-4 h | ASSUMPTION | replica_full, eth3d_office | `TODO` | - | mesh_fscore_5cm, psnr_heldout, atlas_px → mesh F@5cm >= 0.65; textured held-out PSNR >= 18 dB (HANDOFF.md §5 P8, PROPOSED) | P1 | P7-02 |
| P7-04 | COLMAP MVS vs OpenMVS comparison | not_implemented | depends on P7-01/P7-02 | GPU/HIGH_RAM | 8 / 64 / 90 | P7-01 + P7-02 | sum of both | replica_full, eth3d_office | `TODO` | - | fscore_5cm, wall_s → TARGET TBD | P2 | P7-01, P7-02 |

## Phase 8

| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P8-01 | Gaussian Splatting full training (30k iters) per eval scene (gsplat) | not_implemented | trainer not written; gsplat CUDA kernels need NVIDIA GPU | GPU | 16 / 32 / 5 | 0.5-1.5 h per scene | ASSUMPTION: order of magnitude from gsplat/3DGS literature for 30k steps on one high-end GPU; not measured | replica_full, arkitscenes_2 | `TODO (recon/splat)` | - | psnr, ssim, lpips, n_gaussians, peak_vram_gb, wall_s → held-out PSNR >= 24 dB, SSIM >= 0.80, LPIPS <= 0.30 (HANDOFF.md §5 P9, PROPOSED) | P1 | P6-01, P0-02 |
| P8-02 | Splat sanity benchmark: one Mip-NeRF 360 scene vs published numbers | not_implemented | trainer not written; 12.5 GB dataset > free disk | GPU/LARGE_DISK | 16 / 32 / 15 | ~1 h | download MEASURED 12.54 GB (whole zip; no per-scene archive found) | mipnerf360 | `TODO` | - | psnr, ssim, lpips → within TARGET TBD of published gsplat numbers | P1 | P8-01 |
| P8-03 | Splat ablations: AR vs refined vs pure-SfM poses; keyframe selector vs every-Nth | not_implemented | depends on P8-01; CaptureKit keyframe selector does not exist | GPU | 16 / 32 / 15 | 3-6 h | ~3-4 x P8-01 | replica_full, arkitscenes_2 | `TODO` | - | psnr, ssim, lpips → TARGET TBD | P2 | P8-01 |
| P8-04 | Splat compression/export size + quality loss | not_implemented | no export format decided | GPU | 8 / 16 / 2 | < 1 h | ASSUMPTION | - | `TODO` | - | size_mb, psnr_drop_db → TARGET TBD | P2 | P8-01 |

## Phase 9

| id | name | status | why not local | req | VRAM / RAM / disk GB | runtime | basis | datasets | command | outputs | metrics → pass | prio | deps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P9-01 | End-to-end Scan Bundle -> SpatialMap + ReconPackage (mesh + splat separate) on every eval scene | not_implemented | Phase 4, Part 4 and Scan Bundle adapter not implemented | GPU/HIGH_RAM/LARGE_DISK | 16 / 64 / 100 | sum of stages | sum of P2-P8 estimates | replica_full, arkitscenes_2, scanbundles_real | `TODO` | - | wall_s_per_stage, disk_gb_per_run → all outputs schema-valid; hashes verify (HANDOFF.md §5 P10) | P1 | P4-02, P7-03, P8-01 |
| P9-02 | Reproducibility: full pipeline twice on the big machine | not_implemented | depends on P9-01 | GPU/HIGH_RAM/LARGE_DISK | 16 / 64 / 100 | 2 x P9-01 | repeat | replica_full | `TODO` | - | metric_deltas → TARGET TBD | P2 | P9-01 |
| P9-03 | Cross-platform: ARKit vs ARCore bundles of the same room | not_implemented | no real Scan Bundles exist (CaptureKit SDK not implemented) | GPU/HIGH_RAM | 16 / 64 / 40 | 2 x P9-01 | repeat | scanbundles_real | `TODO` | - | fscore_between_platforms → TARGET TBD | P2 | P9-01 |

## Calibration runs (MEASURED locally)

| id | what | values |
|---|---|---|
| C1 | Phase 3 R0 oracle, Replica room0, 100 frames @300x170, voxel 2 cm (fusion+eval) | {'wall_s': '28-33', 'peak_ram_gb': 1.2} (src: runs/bench_phase3/room0/R0/eval.json) |
| C2 | Depth-Anything-V2-S metric, CPU, 518x914 input, batch 4 | {'ms_per_frame': 1600} (src: R2 run, 100 frames in 146 s incl. model load + fusion) |
| C3 | Depth-Anything-V2-S metric, CPU, 518x910 input, batch 1 | {'ms_per_frame': 3301} (src: probe on Replica frame 500) |
| C4 | Phase 2 refine, TUM fr1/xyz 60 frames 640x480, SIFT@640, seq+spatial matching | {'wall_s': 47.6, 'peak_ram_gb': 1.4} (src: runs/test_results/phase2_tum_xyz.json) |
| C5 | Phase 2 refine, ETH3D office 26 imgs 1555x1036, exhaustive, SIFT@1600 | {'wall_s': 17.6, 'peak_ram_gb': 4.3} (src: runs/test_results/phase2_eth3d.json) |
| C6 | Phase 2 refine, Replica room0 80 frames 600x340 | {'wall_s': 17.8, 'peak_ram_gb': 1.2} (src: runs/test_results/phase2_replica_room0_vio_small.json) |
| C7 | Phase 3 R5 full mono pipeline, room0 100 frames @300x170 (predictions cached) | {'wall_s': 27, 'peak_ram_gb': 2.5} (src: runs/bench_phase3/room0/R5/eval.json) |
| C8 | OpenMVS v2.4.0 CPU Docker image | {'image_gb': 0.594} (src: docker images room-openmvs:2.4.0) |
