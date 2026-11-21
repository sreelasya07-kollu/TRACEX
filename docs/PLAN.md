> **FROZEN 2026-11-14.** Historical plan, no longer updated. Open items moved to
> [04-TASKS.md](04-TASKS.md); status lives in [PROGRESS.md](../PROGRESS.md); scope lives in
> the charter ([README.md](README.md) maps who owns what).

# Implementation plan: roomscan-recon (Part 3 `spatial/`, Part 4 `recon/`)

Written 2026-11-14 from PROGRESS.md, HANDOFF.md §5, runs/test_results and the Part 2 build
guide. Status words: **done** = measured evidence in runs/test_results; **partial** = code
exists, acceptance not fully met; **missing** = no code.

## 1. Where the project stands

| Phase (HANDOFF) | Status | Evidence | Gap to "perfect" |
|---|---|---|---|
| P0 environment | partial | E1–E3, E7 pass | E4/E5 need CUDA; E6 ETH3D SfM 20/26 at 1600 px (needs full-res rerun); CUDA Docker image unbuilt |
| P1 ingest + adapters | done | phase1_*.json | Scan Bundle adapter missing (spec not written); synthetic room is an eval generator, not an ingest adapter; keyframe gap check (0.3 m / 15°) not asserted |
| P2 pose refinement | done | phase2_*.json | vio_large scale drift 2.35 % (needs depth-based scale, P3); ETH3D registration intermittent; no vocab-tree / global mapper option |
| P3 depth + scale | partial | phase3_nyu_*.json, bench_phase3 | Sensor path F@5cm 0.946. Mono path F@5cm 0.338 (R5): below any usable bar. MVS depth (OpenMVS/COLMAP) not wired. Scale-recovery test (×0.37 model) not run. Mono on ETH3D gives 0.28 % valid pixels |
| P4 fusion | partial | bench_phase3 (Replica Chamfer) | ETH3D eval-tool F-score never computed (needs MVS or usable mono depth); synthetic Chamfer ≤ 1 cm not asserted as a test |
| P5 planes + room | done | phase5_*.json | ETH3D floor⟂wall check blocked (no gravity); bays/recesses via wall-snapped footprint contour (synthetic L and recess tested, Replica room0 not re-run); scale_confidence null |
| P6 SpatialMap writer | missing | – | – |
| P7–P10 Part 4 (`recon/`) | missing | `recon/__init__.py` only | all of sparse export, OpenMVS dense/mesh/texture, gsplat, packaging |
| Reports | missing | docs/PHASE_REPORTS empty | no per-phase report has been written |
| Version control | **missing** | not a git repo | nothing is committed; one bad `rm` loses everything |

Machine limits that shape the order: 15 GB RAM (~2–3 GB free), 6.7 GB disk free, no CUDA.
Everything below is tagged **CPU** (do here) or **GPU** (needs the GPU machine).

## 2. Immediate hygiene (half a day, CPU)

1. `git init`, commit the current tree (user to confirm; HANDOFF says commit on request).
2. Free disk: `runs/bench_phase3/room0/R2` alone is 577 MB of a known-bad recipe; keep
   `eval.json`/`meta.json`, delete its `fusion/` and `depth_aligned/`. Target ≥ 15 GB free.
3. Write the missing phase reports (P1, P2, P3-partial, P5) in `docs/PHASE_REPORTS/` using
   the HANDOFF §6 template; numbers already exist in `runs/test_results/`.
4. Add `pytest -m light` to a `make check` / `scripts/check.sh` with ruff, so every phase ends
   with one command.

## 3. Phase-by-phase plan

### P3 (finish): depth + scale — 1–2 weeks, mostly CPU
Goal: a depth path that works without a depth sensor, and scale from depth.
- **3a MVS depth (CPU).** Wire OpenMVS `DensifyPointCloud` (existing docker image) as a
  depth source: `spatial/depth/sources.py: MVSDepth` writing per-frame depth + confidence
  from `InterfaceCOLMAP` of `sparse/0`. Test: TUM fr1/xyz MVS depth vs Kinect depth on valid
  pixels, **AbsRel ≤ 0.10, δ<1.25 ≥ 0.90** (HANDOFF P3).
- **3b Depth-based scale.** Fit the Sim(3) scale from sensor/MVS depth against sparse points
  (reuse `fit_scale`); apply to the Replica vio_large case to remove the 2.35 % miss. Test:
  deliberately ×0.37 model → **scale error ≤ 2 %**.
- **3c Mono depth fix.** The 0.338 F-score comes from per-frame scale drift of
  Depth-Anything-Small. Options in order of cost: (i) align mono to MVS/sparse depth per
  frame with scale *and* shift, mask low-confidence; (ii) fuse mono only where MVS has holes
  (the "fusion rule" in the build guide). Target: R5 F@5cm ≥ 0.70 on room0, or document why
  mono stays a fallback only.
- **3d Consistency filter.** On ETH3D it keeps 4 % of pixels: make `rel_thresh`/`min_views`
  depend on baseline, and add a test that a correct wall keeps ≥ 95 % (exists) *and* that a
  real scene keeps ≥ 50 %.

### P4 (finish): fusion evaluation — 3 days, CPU + docker
- Run the ETH3D eval tool (docker `room-eth3d-eval`) on the office fused cloud from MVS
  depth: **F-score@5 cm ≥ 0.60**, report 1/2/5/10 cm.
- Promote the synthetic room into a proper test: Chamfer-L1 of the TSDF cloud vs the GT mesh
  **≤ 1 cm** (generator already in `eval/synthetic_room.py`).
- Add `--max-frames` streaming so a 2000-frame scene fuses under 3 GB RSS (voxel 2 cm).

### P5 (polish): planes + room — 3 days, CPU
- Bays/recesses: done via the wall-snapped footprint contour (synthetic L and recess rooms).
  Still to run: Replica room0 polygon IoU vs the GT-mesh polygon ≥ 0.95 (heavy test added,
  data absent when implemented, not re-run).
- Gravity residual: feed the floor normal back as a frame correction (Replica frame is tilted
  1.74°). Store the correction in `room.json`, do not silently rotate the cloud.
- `scale_confidence`: a documented rule (sensor depth 1.0, metric priors 0.8, SfM-only 0.0)
  plus the Sim(3) inlier ratio; call it a heuristic in the schema description.
- ETH3D check: either add a gravity prior from the GT floor (eval only, clearly labelled) or
  drop the ETH3D criterion in HANDOFF with a written reason.

### P6: SpatialMap writer + CLI — 1 week, CPU
- `spatial/spatial_map.py`: assemble `cameras/`, `sparse/`, `depth/`, `fused_cloud.ply`,
  `tsdf_mesh.ply`, `planes.json`, `room.json`, `quality.json`, `manifest.json` with sha256 of
  every file (reuse `StageRecorder`). JSON Schemas exported from the pydantic models into
  `spec/` and a `docs/interfaces/spatial_map.md`.
- `python -m spatial.cli run --dataset … --src …` chaining P1→P2→P3→P5→P6 (one command).
- Tests: schema-valid; rerun with the same seed gives identical hashes for deterministic
  files; a `formats`-only reader loads it (this is what `recon/` will use).

### P7: Part 4 sparse — 3 days, CPU
- `recon/sparse.py`: load SpatialMap COLMAP model, or run SfM if absent; export
  `InterfaceCOLMAP` scene for OpenMVS and the gsplat init cloud.
- Test: ETH3D office **≥ 95 % registered, reproj < 1.0 px**, scene.mvs loads in docker.

### P8: dense + mesh + texture — 1–2 weeks, CPU (docker)
- `recon/dense.py`: DensifyPointCloud → ReconstructMesh → RefineMesh (optional) →
  TextureMesh via `room-openmvs:2.4.0`; CPU fallback Open3D Poisson; GLB export via trimesh.
- Tests: ETH3D eval tool dense **F@5 cm ≥ 0.70**, mesh-sampled **≥ 0.65**; textured mesh
  rendered at held-out GT views **PSNR ≥ 18 dB** (Open3D offscreen).
- Disk: a DSLR-res OpenMVS run needs ~10–20 GB; downscale 2 locally.

### P9: Gaussian splatting — 1–2 weeks, **GPU**
- `recon/splat/trainer.py` around `gsplat.rasterization` + `DefaultStrategy`, init from the
  SpatialMap cloud, held-out every 8th image, export standard 3DGS PLY (+ SPZ later).
- Tests: ETH3D ½ res **PSNR ≥ 24, SSIM ≥ 0.80, LPIPS ≤ 0.30**; re-render of the exported PLY
  matches the training render (PSNR > 40 dB). Record VRAM, time, #Gaussians.
- Until the GPU machine exists, write the trainer against the gsplat API and the data loader
  tests on CPU (they are marked `gpu` and skip here).

### P10: Reconstruction packaging + end-to-end — 1 week, CPU
- `recon/package.py` + `room-recon run`; manifest hashes; `eval/metrics.json` per output.
- End-to-end `room-spatial run` → `room-recon run` on ETH3D and the synthetic room; record
  wall time and peak RAM/VRAM.

### GPU machine batch (when available)
E4/E5, E6 at full res, `pytest -m gpu`, `pytest -m arkit` (ARKitScenes adapter must be
written first, P1), CUDA Docker build, P9. The inventory (docs/heavy-compute) lists 37 items
with commands; run them in its priority order.

## 4. Beyond HANDOFF: what the Part 2 build guide adds (P11+)

These are the "glue" the guide says nobody ships. Do them after P10, in this order:
1. **Scan Bundle adapter + validator** (guide stage 0): blocked on the CaptureKit spec. Write
   a v0.1 proposal (`manifest.json`, frames, depth, poses, IMU, checksums, three capture
   tiers) with a JSON Schema and one synthetic sample bundle per tier, so Part 3 can be
   tested on it before real captures exist.
2. **QA gate + Job Report** (guide stage 9, spec D-ScanQA): pose quality, coverage per plane
   (observation count per `planes.json` polygon), geometry checks (wall planarity `rms_m`,
   floor⟂wall, closed loop), scale checks, splat held-out metrics; verdict pass / warn /
   rescan with anchored regions. Most inputs already exist in `meta.json` and `room.json`.
3. **Recipe router** (guide §6.3): map requested outputs + tier to P-stages; fallbacks
   (SfM failed → feed-forward poses) only after a feed-forward model (VGGT / MapAnything,
   licence-checked) is added as a GPU worker.
4. **Exports**: GLB (done in P8), USDZ, E57/LAZ, DXF/SVG floor plan from `room.json`
   (ezdxf), versioned semantic JSON (already `room.json`).
5. **Golden dataset + regression gate**: 20–50 rooms with tape measurements; every library
   or model upgrade reruns the benchmark suite and compares F-score, wall offsets, area.
6. **Semantics v2** (openings, objects, SpatialLM): research; only after the above is stable.

## 5. Definition of "perfect" for this repo

- Every HANDOFF P1–P10 acceptance line has a measured number in `runs/test_results/` and a
  report in `docs/PHASE_REPORTS/`, with thresholds never changed without a written reason.
- One command per part (`room-spatial run`, `room-recon run`) runs end-to-end on CPU for
  P1–P8 and reproduces hashes.
- No fabricated metrics, no placeholder outputs; GPU-only items are marked and skipped with
  the reason printed.
- Everything committed, datasets and runs gitignored, licences inventoried (MODELS.md).
