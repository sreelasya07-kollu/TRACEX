# Datasets (benchmark use only; never redistributed, never real user scans)

Fetch with `data/fetch.sh` (+ `data/fetch_replica.py`, `data/extract_nyu.py`). Contents of
`data/` are gitignored except these scripts and this file. Exact frame ids used by each run
are recorded in that run's `workspace/workspace_meta.json` and `phase3/meta.json`.

| Dataset | Licence | What we fetch | Subset used | Used for |
|---|---|---|---|---|
| Replica (NICE-SLAM render) `room0`, `office0` | Replica research licence (Meta, non-commercial research) | every 5th frame of 2000 (400) via HTTP range reads of `Replica.zip`; `<scene>_mesh.ply`; `cull_replica_mesh.zip` | Phase 3: every 4th of the 400 → 100 frames (original ids 0, 20, …, 1980), images downscaled ×2 (600×340), fusion ×2 more (300×170) | Phase 1–2 conventions, Phase 3 oracle (R0), ablation, voxel sweep, robustness |
| TUM RGB-D `fr1/xyz`, `fr1/desk` | CC BY 4.0 | full sequences | Phase 2: 60 frames, stride 10; Phase 3 Step D: 100 frames, stride 5 (fr1/desk) | Phase 2 refinement; Step D mono vs Kinect |
| ETH3D `office` (DSLR undistorted + scan eval) | CC BY-NC-SA 4.0 | 26 images, COLMAP GT, laser scan | all 26, ×4 downscale | Phase 1 conventions, Phase 2 SfM-only accuracy |
| NYU Depth v2 labelled | NYU research use (Silberman et al. 2012) | `nyu_depth_v2_labeled.mat` (deleted after extraction) + `splits.mat` | official 654-image test split | Step A: reproduce published DAv2 numbers |
| ARKitScenes | Apple ML research licence (non-commercial) | **not on this machine** | pending: GPU test machine | R0–R6 on real LiDAR, Step D |
