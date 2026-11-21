# Big-machine results (fill with MEASURED values only; leave blank until run)

Machine: `python scripts/heavy/check_machine.py > machine.txt` (attach). Each row links its
`runs/heavy/<date>/<id>/meta.json`.

| id | name | status (ok/failed/skipped) | wall time | peak VRAM GB | peak RSS GB | disk GB | key metrics | pass? | meta.json |
|---|---|---|---|---|---|---|---|---|---|
| P0-01 | Build CUDA Docker image (COLMAP-CUDA incl. global mapper, pycolmap-cuda12, Open3D-CUDA, PyTorch-CUDA, gsplat, opt. nerfstudio) |  |  |  |  |  |  |  |  |
| P0-02 | Environment GPU acceptance tests E4 (gsplat projection) + E5 (gsplat fit, PSNR > 25 dB) |  |  |  |  |  |  |  |  |
| P1-01 | Ingest round-trip on full-length real Scan Bundles (10-min captures) |  |  |  |  |  |  |  |  |
| P1-02 | Ingest all Replica frames (stride 1, full 1200x680) for room0/office0 |  |  |  |  |  |  |  |  |
| P2-01 | COLMAP GPU SIFT extraction + matching, all keyframes, full resolution |  |  |  |  |  |  |  |  |
| P2-02 | Sequential vs exhaustive vs vocab-tree matching on 500-2000 images |  |  |  |  |  |  |  |  |
| P2-03 | hloc SuperPoint + LightGlue (GPU) vs SIFT |  |  |  |  |  |  |  |  |
| P2-04 | Global SfM (GLOMAP, now COLMAP 4.x 'global' mapper) vs COLMAP incremental |  |  |  |  |  |  |  |  |
| P2-05 | Full-dataset ATE/RPE before vs after refinement (all Replica scenes, ETH3D 13 scenes, TUM fr1/*) |  |  |  |  |  |  |  |  |
| P2-06 | Sim(3) scale-recovery accuracy across all scenes |  |  |  |  |  |  |  |  |
| P3-01 | Mono metric depth (DAv2-S, integrated) on ALL frames of each eval scene, CPU vs GPU fp16 |  |  |  |  |  |  |  |  |
| P3-02 | Depth-Anything-V2 Base/Large metric (CC-BY-NC, opt-in) accuracy + speed |  |  |  |  |  |  |  |  |
| P3-03 | UniDepth / Depth Pro mono depth comparison |  |  |  |  |  |  |  |  |
| P3-04 | Step A NYU reproduction (654 images) vs published DAv2 Table 2 (ViT-S rel: AbsRel 0.053, d1 0.973) |  |  |  |  |  |  |  |  |
| P3-05 | Depth accuracy vs GT before/after sparse alignment, all frames, per model |  |  |  |  |  |  |  |  |
| P3-06 | Scale/shift fitting robustness: RANSAC vs least squares, outlier rates, all frames |  |  |  |  |  |  |  |  |
| P3-07 | Ablation R0-R6 on ARKitScenes (real LiDAR + ARKit poses) |  |  |  |  |  |  |  |  |
| P3-08 | TSDF voxel sweep 1/2/4 cm, full rooms, all frames: CPU ScalableTSDF vs CUDA VoxelBlockGrid |  |  |  |  |  |  |  |  |
| P3-09 | Geometry metrics with >= 1M sampled points per side |  |  |  |  |  |  |  |  |
| P3-10 | Large-scene stress: breaking point (max frames, min voxel) |  |  |  |  |  |  |  |  |
| P3-11 | Determinism: same input twice -> near-identical outputs |  |  |  |  |  |  |  |  |
| P3-12 | Phase 3 GPU tests: CUDA fusion + GPU inference parity with CPU |  |  |  |  |  |  |  |  |
| P4-01 | Planes + room geometry on 2M / 5M / 20M point clouds vs < 30 s target |  |  |  |  |  |  |  |  |
| P4-02 | Full Phase 4 eval suite on every real scene + tape-measured rooms; parameter-frozen final run |  |  |  |  |  |  |  |  |
| P6-01 | Part 4 sparse reconstruction without priors on all eval scenes |  |  |  |  |  |  |  |  |
| P6-02 | Cache/hash behaviour on large runs (skip on re-run, partial re-run after one stage change) |  |  |  |  |  |  |  |  |
| P7-01 | COLMAP patch_match_stereo + stereo_fusion (CUDA-only), full res |  |  |  |  |  |  |  |  |
| P7-02 | OpenMVS DensifyPointCloud (CPU, RAM-heavy) full + half res |  |  |  |  |  |  |  |  |
| P7-03 | ReconstructMesh -> RefineMesh -> TextureMesh full res; texture atlas size; mesh + texture quality |  |  |  |  |  |  |  |  |
| P7-04 | COLMAP MVS vs OpenMVS comparison |  |  |  |  |  |  |  |  |
| P8-01 | Gaussian Splatting full training (30k iters) per eval scene (gsplat) |  |  |  |  |  |  |  |  |
| P8-02 | Splat sanity benchmark: one Mip-NeRF 360 scene vs published numbers |  |  |  |  |  |  |  |  |
| P8-03 | Splat ablations: AR vs refined vs pure-SfM poses; keyframe selector vs every-Nth |  |  |  |  |  |  |  |  |
| P8-04 | Splat compression/export size + quality loss |  |  |  |  |  |  |  |  |
| P9-01 | End-to-end Scan Bundle -> SpatialMap + ReconPackage (mesh + splat separate) on every eval scene |  |  |  |  |  |  |  |  |
| P9-02 | Reproducibility: full pipeline twice on the big machine |  |  |  |  |  |  |  |  |
| P9-03 | Cross-platform: ARKit vs ARCore bundles of the same room |  |  |  |  |  |  |  |  |
