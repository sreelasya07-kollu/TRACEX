# TRACEX — Unified Program Progress

TRACEX combines Part 1 (Mobile Capture) and Parts 2-4 (Spatial Understanding & 3D Reconstruction).

---

## Part 1: Mobile Capture Applications & Engine (`app/` & `core/`)

| Phase | Scope | Status |
|---|---|---|
| 0 | Conventions, recording protocol, raw recorders | Complete |
| 1 | Scan Bundle specification & schemas | Complete |
| 2 | Geometry core, OpenCV conventions, reprojection report | Complete |
| 3 | Replay harness, simulation verification | Complete |
| 4 | Blur metric, keyframe selector, quality governor | Complete |
| 5 | Thermal & battery governor | Complete |
| 6 | Native Android / iOS capture pipelines | Android complete; iOS drafted |
| 7 | Tus upload, bundle validation & dev verifier | Complete |
| 8 | GuideMind: LiveMap, room coverage planner | Complete |
| 9 | Room understanding, surface classifier | Complete |
| 10 | Voice assistant & voice commands | Complete |
| 11 | Samsung One UI 7 redesign, review overlay, redaction | Complete |
| 12 | Field testing & device verification | In progress (docs/capture/DEVICE_TEST_LOG.md) |

---

## Parts 2-4: 3D Reconstruction & Spatial Pipelines (`recon/` & `tools/`)

| Phase | Scope | Status |
|---|---|---|
| 0 | Environment, pinned dependencies, CPU/CUDA tests | Complete (CPU verified) |
| 1 | Dataset adapters (Replica, TUM, ETH3D), keyframe selector | Complete |
| 2 | Pose refinement, metric scale alignment | Complete |
| 3 | Depth filtering, consistency masks, TSDF volumetric fusion | Complete |
| 4 | Synthetic Chamfer & Replica fusion evaluation | Complete |
| 5 | Plane fitting, footprint contour, room shell | Complete |
| 6 | SpatialMap serialization models & schemas | Complete |
| 7 | Sparse hand-over to OpenMVS | Complete |
| 8 | OpenMVS dense point cloud, mesh & texture | Complete |
| 9 | 3D Gaussian Splatting (`gsplat`) | Code complete (GPU target) |
| 10 | Packaging manifest, held-out PSNR evaluation | Complete |
