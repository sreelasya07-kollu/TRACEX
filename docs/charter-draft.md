<!--
  DRAFT of the project charter. It becomes the locked charter only when the owner runs:
    mv docs/charter-draft.md docs/00-PROJECT-CHARTER.md
    (set **Status:** LOCKED, fill Locked on / Approved by)
    bash scripts/lock-charter.sh
  After locking, the charter is never edited or relocked. Scope changes go through
  docs/change-requests/ (proposals) and docs/amendments/AMD-NNN-*.md (approved).
  Claude Code: never write to docs/00-PROJECT-CHARTER.md or docs/.charter.sha256.
  Every claim below cites its source in [brackets]. Facts owned elsewhere are linked, not copied.
  The owner removes this whole DRAFT comment block before running scripts/lock-charter.sh.
-->
# Project Charter

**Status:** DRAFT
**Locked on:** YYYY-MM-DD
**Approved by:** <owner name>

## 1. Project name
roomscan-recon: Room Scanner Part 3 (spatial understanding, `spatial/`) and Part 4
(3D reconstruction, `recon/`). [pyproject.toml; HANDOFF.md title and intro]

## 2. One-line idea
Turn a room walkthrough (phone video with or without phone poses, or a dataset capture) into
a metric-scaled, gravity-aligned room model: planes and room shell, a textured mesh and a
Gaussian splat, plus a QA verdict and exports. [docs/specs/C-SplatForge.md §1; HANDOFF.md §4;
docs/PLAN.md §4]

## 3. The problem
Every reconstruction step already exists as a tool (COLMAP, depth models, gsplat, TSDF and
Poisson meshing). What is missing is the glue: choosing a path per scene, detecting failure,
recovering true scale, and producing a clean room-scale result without manual tuning.
[docs/specs/C-SplatForge.md §2; docs/PLAN.md §4 intro]

## 4. Target users
- Internal: the Room Scanner program, consuming Scan Bundles from Project A (CaptureKit) and
  feeding Projects D, E and F. [docs/specs/C-SplatForge.md §3, §13; docs/specs/00-Master-Portfolio.md §3]
- External: shops (product 3D), real-estate teams, creators, researchers, other apps through
  an API. [docs/specs/C-SplatForge.md §3]

## 5. Goals (what success looks like)
1. Every HANDOFF P1-P10 acceptance line has a measured number in `runs/test_results/` and a
   report in `docs/PHASE_REPORTS/`; thresholds are those in HANDOFF.md §5 and change only
   with a written reason. [docs/PLAN.md §5; HANDOFF.md §5]
2. One command per part (PLAN names them `room-spatial run` / `room-recon run`; today
   `python -m spatial.cli run` / `python -m recon.cli run`) runs end-to-end on CPU for P1-P8
   and reproduces hashes. [docs/PLAN.md §5; HANDOFF.md §2 rule 5, §5 P10]
3. Metric scale comes with a confidence value, from layered sources (phone VIO, sensor or
   MVS depth, a reference length). [docs/specs/C-SplatForge.md §6, §8; docs/PLAN.md §3 P5]
4. Splat for looks, mesh for measuring, both from one run and neither derived from the other.
   [docs/specs/C-SplatForge.md §6; HANDOFF.md §2 rule 2]

## 6. Core features (MUST be built: this is the scope)
1. Ingest: `CaptureInput` adapters (ETH3D, TUM RGB-D, Replica, synthetic), keyframe
   selection, and a Scan Bundle v0.1 adapter + validator. [HANDOFF.md §4.1, §5 P1;
   docs/PLAN.md §4 item 1; docs/specs/C-SplatForge.md §5 P0]
2. Pose refinement to a metric, gravity-aligned sparse model, with an SfM fallback when no
   poses exist. [HANDOFF.md §5 P2; docs/specs/C-SplatForge.md §5 P2-P3, §6]
3. Depth and scale: sensor depth, MVS depth, mono depth as fallback; scale recovery.
   [HANDOFF.md §5 P3; docs/specs/C-SplatForge.md §5 P3-P4]
4. Fusion into a cleaned point cloud and TSDF mesh. [HANDOFF.md §5 P4;
   docs/specs/C-SplatForge.md §5 P5]
5. Planes and room shell: floor, ceiling, walls, floor polygon, ceiling height.
   [HANDOFF.md §5 P5]
6. SpatialMap on-disk format, writer, schemas and `spatial` CLI (Part 3 output, Part 4
   input). [HANDOFF.md §4.2, §5 P6]
7. Part 4 sparse hand-over, OpenMVS dense cloud, mesh and texture, GLB export.
   [HANDOFF.md §5 P7-P8; docs/specs/C-SplatForge.md §5 P5, P7]
8. Gaussian-splat training and standard 3DGS PLY export as a separate output.
   [HANDOFF.md §2 rule 2, §5 P9; docs/specs/C-SplatForge.md §5 P6]
9. Reconstruction packaging, manifest with hashes and `recon` CLI. [HANDOFF.md §4.3, §5 P10]
10. QA gate and Job Report (`qa/`): pass / warn / rescan verdict with anchored rescan regions.
    [docs/PLAN.md §4 item 2, in PLAN since root commit b9fc6e4; owner decision 2026-11-17,
    overrides C-SplatForge §4 'Out for v1']
11. Recipe router (`router/`): requested outputs + capture tier to stages, with fallbacks.
    [docs/PLAN.md §4 item 3; docs/specs/C-SplatForge.md §7 item 2 (Pose Router)]
12. Exports (`export/`; GLB from `recon/`): GLB, USDZ, E57/LAZ, DXF/SVG floor plan, versioned
    semantic JSON.
    [docs/PLAN.md §4 item 4]
13. Golden dataset and regression gate: tape-measured rooms, rerun on every library or model
    upgrade. [docs/PLAN.md §4 item 5]

## 7. Non-goals (we will NOT build these)
- Capture SDK, live guidance and privacy tooling (Projects A, B, I). [HANDOFF.md §2 rule 7]
- Backend platform: accounts and queues (Project G). [docs/specs/C-SplatForge.md §4
  "Out for v1"]
- Viewer (Project F). [docs/specs/C-SplatForge.md §4 "Out for v1"]
- Semantics v2 (openings, objects, SpatialLM). [docs/PLAN.md §4 item 6; owner decision
  2026-11-17]
- Fake algorithms, placeholder outputs or fabricated metrics. [HANDOFF.md §2 rule 4]

## 8. Constraints
- Architecture: `spatial/` and `recon/` are independent and exchange data only through the
  on-disk formats. [HANDOFF.md §2 rule 1; docs/INTERFACES.md §9]
- Reuse mature libraries; do not re-implement them. [HANDOFF.md §2 rule 3]
- Platforms: CPU must work for everything except splat training; GPU is optional
  acceleration. Python range and pins: see `pyproject.toml` / `uv.lock`. [HANDOFF.md §2 rule 5]
- Keep every intermediate result and processing metadata. [HANDOFF.md §2 rule 6, §4.4]
- One phase at a time, measured tests only, no completion claim without reproducible
  evidence. [HANDOFF.md §2 rules 8-10]
- Legal: permissive building blocks by default; model, tool and dataset licences are listed
  in docs/MODELS.md. [docs/specs/C-SplatForge.md §6, §11; docs/specs/00-Master-Portfolio.md §8]

## 9. Success criteria / definition of "done" for the whole project
- [ ] Every HANDOFF.md §5 acceptance line (P1-P10) has a measured number and a phase report.
  [docs/PLAN.md §5]
- [ ] The Part 3 and Part 4 run commands run end-to-end on CPU for P1-P8 and reproduce
  hashes. [docs/PLAN.md §5]
- [ ] GPU-only items are marked and skipped with the reason printed. [docs/PLAN.md §5]
- [ ] Everything committed, datasets and runs gitignored, licences inventoried in
  docs/MODELS.md. [docs/PLAN.md §5]

## 10. Amendment rule
This charter is locked once and never edited or relocked. A scope change starts as a
proposal in `docs/change-requests/`. Once the owner approves it, it becomes a numbered
amendment `docs/amendments/AMD-NNN-*.md`. Current scope = this charter + all approved
amendments. [owner decision 2026-11-17]
