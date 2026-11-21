# Part 2 — 3D Processing & Reconstruction (Backend)

> A processing system that receives the Scan Bundle and converts it into 3D representations according to the
> user's requirements. It handles geometry reconstruction, depth estimation, point clouds, meshes, Gaussian Splatting
> and semantic room modelling, producing the required final 3D outputs.

**Not part of this repository.** Part 2 is built in the separate repository `roomscan-recon`. This file is kept
here only as a contract reference, so Part 1 decisions (bundle, conventions) stay compatible.

## 1. Input contract
- A Scan Bundle that Part 1's server verifier marked COMPLETE (`capturekit/spec/scanbundle/`, conventions.md).
- The user's requested outputs (e.g. splat only; mesh + floor plan; point cloud for CAD) as a job request.
- Phone poses are a strong prior, not ground truth. Part 1 labels, coverage map and guidance log are hints.

## 2. Pipeline (each stage a container with a defined input/output, retryable and benchmarkable alone)
| Stage | Job | Typical tools |
|---|---|---|
| P0 Ingest validator | Re-validate bundle, frame count, pose coverage | `ckit validate` |
| P1 Frame preprocessor | Undistort, filter blur/exposure, person masks, respect redaction manifest | OpenCV, SAM 2 |
| P2 Pose refinement | Refine phone poses, sparse points; fall back when it fails | COLMAP, GLOMAP, VGGT, MASt3R |
| P3 Scale and gravity | Metric scale, gravity alignment (phone frame is already Y-up metres) | custom |
| P4 Depth estimation | Monocular depth aligned to sparse points | Depth Anything V2, Depth Pro, MoGe |
| P5 Point cloud / dense fusion | Dense point cloud | OpenMVS, COLMAP dense |
| P6 Gaussian Splatting | Trained splat with depth priors | gsplat, splatfacto |
| P7 Mesh / surface | Room-shell and object meshes | TSDF, Poisson, 2DGS |
| P8 Cleanup | Floaters, crop to room, floor alignment, hole filling, simplification | Open3D, trimesh |
| P9 Quality gate | Held-out-view PSNR/SSIM/LPIPS, reprojection, holes; pass/fail; weak regions → RescanRequest | torchmetrics |
| P10 Semantic room model | Floor/walls/ceiling, doors/windows, objects with masks, measurements with uncertainty, floor plan | RANSAC, SAM 2, detectors |
| P11 Packaging and export | Requested formats (PLY, SPZ, GLB, OBJ, USDZ, SVG/PDF plan, JSON), thumbnails, LOD | converters |

## 3. Platform services
Auth and per-user isolation; upload service (tus, checksum verify — Part 1's dev server is the reference); job
orchestrator with retries, timeouts, fallbacks; GPU worker pool; asset storage with signed URLs; notifications
(scan ready, rescan needed); dialog proxy for Part 1's optional LLM/TTS calls; retention, deletion, audit log;
observability and per-job cost.

## 4. Outputs back to Part 1
QualityReport and RescanRequest (weak regions in the bundle's world frame) so the app can guide a revisit.

## 5. Evaluation
Reference rooms with tape-measured ground truth; benchmark runner (quality, scale error, runtime, cost);
golden-bundle regression; guided-vs-unguided reconstruction success (shared study with Part 1).
