# Room Scanner Program: two parts

**This repository (2D-3D) is Part 1 only.** Part 2 is built in a separate repository, `roomscan-recon`.

The program has **two parts**. The earlier split into nine projects (A–I) is superseded; those documents are kept
only as source material in `legacy-nine-projects/` (do not plan from them).

| Part | What it is | Spec | Code |
|---|---|---|---|
| **Part 1 — Intelligent Room Capture (Mobile App)** | Native iOS and Android applications with an advanced AI that understands the room like a human surveyor. It identifies walls, furniture, surfaces, obstacles and missing areas while the user scans. It continuously evaluates capture quality and guides the user with specific instructions, asking them to revisit areas whenever more information is needed. It produces a complete, validated Scan Bundle. | [part1-room-capture.md](part1-room-capture.md) | this repository: `capturekit/` (phases in `docs/phases/`) |
| **Part 2 — 3D Processing & Reconstruction (Backend)** | A processing system that receives the Scan Bundle and converts it into 3D representations according to the user's requirements. It handles geometry reconstruction, depth estimation, point clouds, meshes, Gaussian Splatting and semantic room modelling, producing the required final 3D outputs. | [part2-reconstruction.md](part2-reconstruction.md) (contract reference) | separate repository `roomscan-recon` |

**The contract between them is the Scan Bundle** (`capturekit/spec/scanbundle/`) plus the frozen
[conventions.md](conventions.md). Part 2 reads Part 1 output only through that schema. The only data flowing back is
an optional RescanRequest (weak regions found after reconstruction) that Part 1 can show as "revisit" targets.

## Where the old projects went
| Old project | Now |
|---|---|
| A CaptureKit (capture SDK, bundle, upload, governor, geometry) | Part 1 |
| B GuideMind B1–B3 (coverage, planner, voice, dialog, overlay, review UI) | Part 1 |
| B GuideMind B4 spin-offs (Ask-the-Video, bots, Task Packs) | out of scope (later products) |
| C SplatForge (poses, depth, dense fusion, splats, meshes) | Part 2 |
| D ScanQA (cleanup, quality gate, weak regions → RescanRequest) | Part 2 |
| E RoomSense (layout, openings, objects, measurements, floor plan) | Part 2 (full semantic model); Part 1 keeps the live, on-device subset |
| F SplatView (packager, format converters, viewer) | Part 2 (export of final outputs); viewer is a client of Part 2 |
| G ScanCloud (auth, upload service, jobs, GPU workers, dialog proxy) | Part 2 (Part 1 owns only the upload client and the dev tus server) |
| H ScanBench (replay, golden scans, guided-vs-unguided study) | split: capture/guidance replay and trials → Part 1; reconstruction benchmark → Part 2 |
| I PrivacyShield (consent, on-device redaction, retention, audit) | split: consent + on-device redaction → Part 1; storage, retention, audit → Part 2 |
