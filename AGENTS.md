# AGENTS.md — CaptureKit Part 1 (stable prefix; keep identical across sessions)

Start each session with: "Do Phase N. Read AGENTS.md, docs/phases/NN-*.md and PROGRESS.md only."
One phase per session, in order. Phase N+1 starts only after Phase N acceptance passes.
Numeric thresholds in phase files are starting guesses: tune on recorded data.

## A. Role and context
**This entire repository (2D-3D) is Part 1 only.** Part 2 lives in a separate repository
(`roomscan-recon`); never add Part 2 code, plans or phases here.
The program has exactly TWO parts (docs/program-spec/README.md). The old nine-project split (A–I) is
superseded; never plan from docs/program-spec/legacy-nine-projects/.
- **Part 1 — Intelligent Room Capture (Mobile App)** = this whole repo (code in `capturekit/`). Native iOS + Android app
  with an AI that understands the room like a human surveyor: identifies walls, furniture, surfaces,
  obstacles and missing areas while the user scans, evaluates capture quality continuously, guides with
  specific instructions (voice first) incl. revisits, and produces a complete, validated Scan Bundle.
  Spec: docs/program-spec/part1-room-capture.md. Phone camera only, NO LiDAR.
- **Part 2 — 3D Processing & Reconstruction (Backend)**: receives the Scan Bundle and produces the requested
  3D outputs (poses, depth, point clouds, meshes, Gaussian Splatting, semantic room model). Spec:
  docs/program-spec/part2-reconstruction.md (contract reference only). Built in `roomscan-recon`, not
  here; the Scan Bundle is the only contract.
You are a senior engineer on Part 1. Phases 0–12 in docs/phases/; state is in PROGRESS.md.

## Repo map (paths relative to repo root)
| Path | Purpose | Entry |
|---|---|---|
| capturekit/core | C++20 core: geometry/ bundle/ quality/ governor/ replay/ session/ capi/ (Eigen, Sophus, MCAP, nanobind) | core/CMakeLists.txt |
| capturekit/core/bindings | nanobind module | — |
| capturekit/ios | Swift pkg CaptureKit (iOS 17+, TUSKit, GRDB, Sentry) + SampleApp | ios/Package.swift |
| capturekit/android | Kotlin, ARCore, Room, WorkManager, NDK; :capturekit, :sample-app | android/settings.gradle* |
| capturekit/tools | Python CLI `ckit` (uv, pydantic, mcap, pycolmap, opencv) | tools/ckit/cli.py |
| capturekit/spec/scanbundle | Bundle spec, message schemas, examples | spec/scanbundle/README.md |
| capturekit/dev-server | tusd + MinIO (docker compose) | dev-server/ |
| capturekit/docs/decisions | ADRs NNNN-title.md (this is "docs/decisions/" in phase files) | — |
| capturekit/bench | Fixtures (Git LFS), thermal logs, calibration | — |
| docs/program-spec | README.md (two parts), part1-room-capture.md, part2-reconstruction.md, conventions.md | — |
| docs/program-spec/conventions.md | Frozen conventions (written in Phase 0) | — |
| docs/program-spec/legacy-nine-projects | Superseded A–I docs; source material only | — |
| docs/phases/NN-*.md | One file per phase | — |
| PROGRESS.md | Only memory between sessions | — |

## B. Working rules
1. Read the relevant spec sections and the code you will touch before editing. Explore no further.
2. Tests first where the phase says so. CI green at every commit.
3. Stay inside the current phase. No unrelated refactors.
4. Ambiguity: write capturekit/docs/decisions/NNNN-title.md (3–6 lines), then continue. Don't stop to ask.
5. Small, reviewable commits with clear messages.
6. The core never reads the system clock; time comes only from event t_ns.
7. No public API takes/returns a bare 4x4 matrix without a frame-naming wrapper (T_world_from_camera).
8. Never block AR callbacks. Bounded queues; log every deliberate drop.
9. Final report ≤25 lines: what changed, tests added, commands, acceptance checks run + results, open risks.
10. End of phase: update PROGRESS.md (template below).

## C. Frozen cross-cutting decisions
- World frame: Y-up, gravity-aligned, metres.
- Internal camera frame: OpenCV (X right, Y down, Z forward). ARKit/ARCore/OpenGL look down -Z, Y up:
  convert once at the SDK edge. COLMAP stores world-to-camera, quaternion w,x,y,z.
- Pose: T_world_from_camera = translation + unit quaternion; on disk [w,x,y,z]. Eigen coeffs() is
  [x,y,z,w] — guard against mixing.
- Time: one monotonic ns clock (t_ns) mapped at the edge; verify per-device offsets.
- Depth: optional topic only (ARCore Depth API). Nothing may require dense depth.
- Tracking: ARKit / ARCore only. No custom SLAM. Android = ARCore-capable devices only.
- Bundle: MCAP (Protobuf channels) for time series + separate indexed JPEG keyframes. JSON only for
  manifest.json and camera.json. (Supersedes video.mp4 in the legacy CaptureKit doc — see ADR 0001.)
- Architecture: native shells (Swift, Kotlin) over shared C++ core via a small stable C API. No
  cross-platform UI framework.
- Upload: tus. User instructions: templated vocabulary, no LLM in the real-time loop.

## E. Token-economy protocol
**Session:** one phase per session; end with a handoff in PROGRESS.md and start fresh. Load only the
active phase file. PROGRESS.md ≤40 lines.
**Read less:** search first (rg), read only needed line ranges; never cat large files or trees. Don't
re-read unchanged files. Read spec docs by section (part1-room-capture.md §N as each phase names). Never open
binaries/MCAP/JPEGs in context — use `ckit inspect-*` / `ckit validate` summaries.
**Write less:** targeted edits over rewrites. Generate (pydantic from schemas, protobuf codegen,
bindings from one definition, table-driven tests, shared helpers). One C++ implementation, never
duplicated in Swift and Kotlin. Docs short; ADRs 3–6 lines. No narration; report results and blockers.
**Run less:** narrowest test first; full suite once per milestone; CI-equivalent only before the final
commit. Cap output (quiet flags, `| tail -n 40`, failing tests only; long logs to file). Tiny
fixtures; full golden sessions only for Phase 3+ regression. `replay --fast` with fixed seeds; diff
reports. After 3 failed attempts at one fix: log it in PROGRESS.md Gotchas, record the assumption,
move to the next independent item.
**Deliberate less:** decisions in C and the phase files are made; deviate only when a test or
measurement contradicts them, and record why in an ADR. Plan ≤10 lines per milestone. Simple baselines
first.
**Routing:** cheaper model for boilerplate, bindings, config, docs, fixtures, mechanical refactors.
Strongest model for geometry conventions, validator semantics, concurrency/crash-safety, planner and
coverage logic, debugging reprojection failures. iOS and Android adapters: separate parallel sessions,
each with only its phase file and the C API header.
**Never economize on:** tests-first in Phases 1, 2, 4, 5; projection-invariance and reprojection
checks (Phase 2); crash-safety and forced-kill recovery tests (Phases 6, 7); running acceptance checks
(never summarize from memory); determinism checks (replay twice, byte-identical); ADRs.

### PROGRESS.md template
Done (phases/milestones + commit hashes) · Decisions (pointers to ADRs) · Commands (working one-liners)
· Gotchas · Next (single next milestone)
