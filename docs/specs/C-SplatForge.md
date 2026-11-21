# Project C: SplatForge
*Part C of the Room Scanner Program · Reconstruction pipeline · Prepared November 12, 2026*

## 1. Pitch
A video-to-3D reconstruction service. It takes a phone video (with or without phone poses) and returns a metric-scaled, gravity-aligned Gaussian splat plus an approximate mesh. It picks the best method for each capture, notices when a method fails, and falls back automatically.

## 2. Why it stands alone
Every step of reconstruction already exists as a tool: COLMAP, GLOMAP, VGGT, depth models, gsplat, Nerfstudio, TSDF and Poisson meshing. What does not exist off the shelf is the glue: choosing a path per scene, detecting failure, recovering true scale, and producing a clean room-scale result without manual tuning. That glue is SplatForge. Anyone with a video can use it, not only the room scanner.

## 3. Users and customers
- **Internal:** the room scanner (bundles from Project A; quality checks from Project D).
- **External:** shops (product 3D), real-estate teams, creators, researchers, other apps through an API.

## 4. Scope
**In for v1:** indoor scenes from video; pose estimation with fallback; metric scale; depth priors; splat training; mesh extraction; stage-level logs and cost.

**Out for v1:** quality scoring and cleanup (Project D), semantic understanding (E), viewer (F), accounts and queues (G).

## 5. Pipeline stages
Each stage is a container with a defined input and output so it can be swapped, retried or benchmarked alone.

| Stage | Input to output | Tools | Tag |
|---|---|---|---|
| P0 Ingest Validator | Raw upload to validated bundle (file integrity, frame count, pose coverage) | Custom | Build |
| P1 Frame Preprocessor | Keyframes to cleaned frames (undistort, blur and exposure filter, person masks) | OpenCV, SAM 2 | Integrate |
| P2 Pose Estimator | Frames and phone poses to refined poses and sparse points | COLMAP, GLOMAP, VGGT, optionally MASt3R | Integrate |
| P3 Scale and Gravity Aligner | Poses to metric-scaled, gravity-aligned scene | Custom | Build |
| P4 Depth Estimator | Frames to depth aligned to sparse points | Depth Anything V2, Depth Pro, MoGe | Integrate |
| P5 Dense Fusion (optional) | Poses and frames to dense point cloud | OpenMVS, COLMAP dense | Integrate |
| P6 Splat Trainer | Poses, frames, depth priors to trained splat | gsplat, Nerfstudio splatfacto | Integrate |
| P7 Surface Extractor | Splat or depth maps to room-shell mesh | Open3D TSDF, Poisson, 2D Gaussian Splatting | Integrate |

## 6. Key design decisions
- **Fast path first, fallback second.** Start from phone poses as a prior and refine with bundle adjustment; if that fails the checks, run full structure-from-motion; if that fails, try a feed-forward model such as VGGT.
- **Metric scale comes from layered sources:** phone visual-inertial scale, a reference object, or a user-entered length such as a door height.
- **Splat for looks, mesh for measuring.** Produce both from one run.
- **Use permissive building blocks by default** (see licensing below).
- **Presets per scene type** (small room, large room, product, corridor) rather than one fixed configuration.

## 7. Spin-off products
1. **Video-to-3D API** with scene presets (room, product, vehicle, corridor) and a job interface.
2. **Pose Router (open-source library).** Given a bundle, it chooses between phone-prior refinement, COLMAP, GLOMAP and VGGT, checks the result, and falls back automatically.
3. **Scale-from-Anything.** Recovers metric scale from phone tracking, a door, an A4 sheet, a card, or one typed measurement, and returns a confidence value.
4. **Gravity and Floor Aligner** library.
5. **Blank-Wall Booster:** depth-prior and normal regularisation presets that help textureless walls and ceilings.
6. **Recipe Registry:** tuned parameter presets per scene type, with the benchmark scores that justify each.
7. **Express and Pro modes:** a cheap fast result and a slower high-quality one, with a published quality-versus-cost curve.
8. **Incremental reconstruction:** partial models mid-scan that feed richer feedback to Project B (the hybrid architecture option).
9. **Mirror and Glass Handler:** detects reflective surfaces and masks or flags them.
10. **Reproducible pipeline containers** that other labs can run.

## 8. Achievements to aim for (verify prior art before claiming a first)
- A pose router with a learned failure predictor, evaluated against always using one method.
- Metric-scale estimates that come with a calibrated confidence value, checked against tape-measured rooms.
- Phone-pose-plus-SfM fusion benchmarked against either alone on blank walls and fast motion.
- Appearance embeddings for exposure drift as a default setting, with measured flicker reduction.
- Room-shell meshes from splats with no manual cleanup.
- Mid-scan partial reconstruction that measurably improves live guidance.

## 9. Roadmap
| Stage | Deliverable | Exit criterion (proposed) |
|---|---|---|
| C0 Feasibility, 2 to 4 weeks, no app | 10 to 20 phone-video rooms through COLMAP or GLOMAP plus gsplat, and separately VGGT | Failures, runtimes and quality recorded per room |
| C1 One-worker pipeline | P0, P2, P6 and a packager on one GPU machine | First end-to-end room model from a bundle |
| C2 Stage containers and manifests | Each stage runs alone with schemas | Any stage can be rerun from stored inputs |
| C3 Pose Router and fallback | Automatic path choice and failure checks | Higher unattended success rate than a single method |
| C4 Scale and gravity | P3 | Scale error measured in centimetres on reference rooms |
| C5 Depth priors | P4 plus regularisation | Better blank-wall results on the benchmark |
| C6 Mesh | P7 | Room-shell mesh usable for measurement |
| C7 Incremental mode | Partial models during capture | Latency and cost acceptable in a field test |

## 10. Metrics
Held-out-view PSNR, SSIM and LPIPS; reprojection error; tracking loss; scale error in centimetres; unattended success rate; runtime and cost per scan; fallback rate per stage.

## 11. Licensing watchlist (recollection, not legal advice; verify each license)
The original Inria 3DGS code is non-commercial, while gsplat is Apache-2.0. COLMAP is permissively licensed. DUSt3R and MASt3R weights are non-commercial. VGGT weights have had usage restrictions with a separate commercial checkpoint. Depth Anything V2 small is Apache-2.0 while larger models are non-commercial. SAM 2 is Apache-2.0. Datasets such as ScanNet++ and ARKitScenes have their own terms.

## 12. Risks and mitigations
Blank walls and ceilings (depth priors, capture guidance from B); mirrors and glass (detect and flag); moving people (masks, ask users to clear the room); drift and loop closure (ask for start-point overlap, run loop checks); scale error (confidence value, user calibration); GPU cost (express mode, per-job caps from G).

## 13. Interfaces with other projects
**In:** Scan Bundle (A); job and stage manifests (G); redaction manifest (I). **Out:** splat, mesh, poses and scale to D, E and F; asset manifest to F; benchmark results to H.

## 14. Team and first 30 days
A strong 3D vision engineer plus part-time backend help. Week 1: collect 10 rooms with ordinary phone video. Week 2: COLMAP or GLOMAP plus gsplat on all rooms. Week 3: VGGT on the same rooms. Week 4: write up failures and pick the first pose-router rules.
