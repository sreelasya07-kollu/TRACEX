# Phase 9: Room understanding (surfaces, openings, objects, missing areas)
**Goal/why:** Part 1 must identify walls, furniture, surfaces, obstacles and missing areas while the user scans. Phase 8 measures coverage only on detected planes; this phase adds labels and finds what AR never detected.
Read docs/program-spec/part1-room-capture.md §2.2–2.3.

## Decisions
- Geometry first, ML optional: plane classification from normal, height and extent; ARKit plane classification where supported; on-device detector behind a flag. Rejected: requiring ML for guidance.
- Detector runs in the native shell (Core ML / LiteRT) on keyframes at a governed rate; only boxes/masks + confidence enter the C++ core. Rejected: an ML runtime inside the core.
- Missing areas from a room-shell hypothesis (floor polygon, walls extended to corners, ceiling height) minus observed cells. Rejected: coverage of detected planes only (blind to never-detected surfaces).
- Labels and the room shell go into the bundle as new channels (ADR + spec minor version bump). Rejected: app-only state Part 2 never sees.

## Tests first
- Synthetic rooms (box, L-shape, furniture blocks, door and window gaps): classification accuracy per plane type, opening detection, shell corners within tolerance.
- Missing areas: hide one wall / one corner / the space behind a block in a replay; the gap must be reported with the right region and label.
- Lifting: synthetic 2D boxes over known 3D blocks → 3D boxes within tolerance; no labels → guidance unchanged (Phase 8 regression stays green).

## Build
1. SurfaceClassifier (floor / wall / ceiling / horizontal surface) with confidence; ARKit classification mapped at the edge where present.
2. OpeningDetector: rectangular gaps in wall planes and point-free regions bounded by wall points → door / window candidates.
3. ObjectLifter: detections (class, box, score, t_ns, frame) + sparse points/planes → 3D oriented boxes; tracks merge across frames; obstacles feed NavGrid.
4. Model choice and conversion: one small detector exported to Core ML and LiteRT (indoor furniture classes; licence checked), benchmark latency per tier, adapters behind a flag. ADR for the model.
5. RoomShell + GapFinder: shell hypothesis updated live; expected-but-unobserved surfaces, corners and occluded zones as regions with reasons; LandmarkLabeler names regions for instructions.
6. Bundle channels /guide/labels, /guide/shell, /guide/gaps (schemas, validator rules, `ckit inspect` support).

## Constraints
Deterministic for a given input stream (detections are recorded inputs in replay). Detector duty cycle obeys the governor. Average core update ≤10 ms.

## Acceptance
Stage tests pass. Missing-area recall and false-gap rate reported on golden sessions vs hand labels (2–3 sessions). Detector latency table per device tier (device work blocked on hardware).
