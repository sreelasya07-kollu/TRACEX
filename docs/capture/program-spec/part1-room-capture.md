# Part 1 — Intelligent Room Capture (Mobile App)

> Native iOS and Android applications with an advanced AI that understands the room like a human surveyor. It
> identifies walls, furniture, surfaces, obstacles and missing areas while the user scans. It continuously evaluates
> capture quality and guides the user with specific instructions, asking them to revisit areas whenever more
> information is needed. It produces a complete, validated Scan Bundle.

**This whole repository is Part 1.** Code name: **CaptureKit** (`capturekit/`). The guidance engine inside it is **GuideMind** (`capturekit/core/guide/`).
Part 1 ends when a validated Scan Bundle is verified on the server. Everything after that is Part 2.

## 1. Fixed constraints
- Phone camera only, **no LiDAR**. Tracking comes from ARKit / ARCore; no custom SLAM. Depth is an optional hint.
- Native shells (Swift, Kotlin) over one shared C++ core behind a small C API. No cross-platform UI framework.
- The surveyor "brain" is deterministic and testable by replay: the planner decides *what* to do; language only
  phrases it. **No LLM in the real-time loop.** Urgent lines use local templates and local text-to-speech.
- On-device ML (detectors, segmenters) is optional behind flags. Guidance must still work with it off.
- Frame conventions: [conventions.md](conventions.md). Bundle: `capturekit/spec/scanbundle/`.

## 2. Capabilities (mapped to the definition)
### 2.1 Capture foundation
AR session adapter (ARKit / ARCore, sensor-oriented pose, intrinsics, tracking state, feature points, planes);
sensor logger (gyro, accel); camera controller (high-resolution stills, exposure/focus lock where allowed); session
state machine with crash recovery; crash-safe frame store; thermal and battery governor; shared geometry library.

### 2.2 Understands the room ("identifies walls, furniture, surfaces, obstacles")
| Component | Job |
|---|---|
| LiveMap | Fuses sparse points and plane anchors into a gravity-aligned room frame; merges duplicate planes; room extents |
| Surface classifier | Labels planes floor / wall / ceiling / table-like surface. Geometry first; ARKit plane classification where supported; Android needs its own |
| Opening detector | Doors and windows on wall planes (geometry gaps + optional detector) |
| Object detector + lifter | On-device detector/segmenter (Core ML / LiteRT) for furniture and obstacles; lifts 2D detections onto sparse points/planes into 3D object boxes with labels |
| NavGrid | Floor occupancy: free space, obstacles, user position, so guidance never points into furniture or stairs |
| Landmark labeler | Names regions ("the window wall", "behind the sofa") for instructions |

### 2.3 Finds missing areas
A plane-only coverage map cannot see what AR never detected. Part 1 therefore keeps a **room-shell hypothesis**
(floor polygon + wall planes extended to corners + ceiling height) and marks **expected but unobserved** surfaces,
corners and occluded zones (behind/under objects) as gaps, alongside covered-but-weak cells. Low-texture walls get
low-confidence cells instead of guesses.

### 2.4 Evaluates capture quality continuously
Per-frame sharpness, gyro blur risk, exposure clipping, corners/texture; keyframe selection by quality + novelty;
per-cell coverage (distinct views, angular spread, range, incidence, sharpness); tracking-state and relocalisation
handling; governor policy. All produce reason codes the guidance layer can speak.

### 2.5 Guides the user ("specific instructions … revisit areas")
| Component | Job |
|---|---|
| Planner | Stage machine: calibrate → perimeter sweep → detail pass → revisit gaps → loop closure → review. Frontier / next-best-view scoring (coverage gain − travel − rotation − safety), hysteresis, rule-based fallback |
| Instruction generator | Fixed vocabulary with parameters (slow_down, hold_still, step_back, move_left/right, tilt_up/down, rotate_toward(target), revisit(region), finish); priorities, cooldowns, no back-to-back repeats; quality interrupts |
| Renderers | Voice (text-to-speech), tones, haptics, AR overlay (coverage heatmap, arrows, progress ring) |
| Voice assistant | On-device speech recognition for commands; barge-in and echo handling; intents: pause, resume, repeat, skip, how much is left, what's missing, I'm done |
| Grounded Q&A (optional, non-urgent) | Phrasing and questions answered only from structured scan state via a backend dialog proxy (no keys on phone); cites regions; says when it cannot tell; template fallback offline |
| Safety rules | Never guide into stairs/obstacles; footing reminders; never ask to walk backwards blindly |
| Review and rescan | End-of-scan summary, weak-area thumbnails, tap-to-rescan, guided return to a spot; can also show a RescanRequest from Part 2 |

### 2.6 Produces a complete, validated Scan Bundle
Finalize (close streams, sha256 every file, on-device validator, manifest last), plain-language verdicts,
resumable tus upload (manifest last), server-side verification (COMPLETE / INCOMPLETE / CORRUPT). The bundle also
carries Part 1's understanding for Part 2: coverage map, labels/objects, guidance log (schema addition by ADR).

### 2.7 Trust
Onboarding and permissions (camera, motion, microphone), versioned consent recorded in the bundle, on-device
redaction of faces and screens before upload (view-consistent), telemetry and crash reporting.

## 3. Build plan
Phases live in `docs/phases/NN-*.md`, one per session, in order:
0 conventions + recording · 1 Scan Bundle spec · 2 geometry core · 3 replay harness · 4 quality + keyframes ·
5 thermal governor · 6 native capture · 7 finalize + upload · 8 guidance core (LiveMap, coverage, NavGrid, planner,
instructions) · 9 room understanding (surfaces, openings, objects, missing areas) · 10 voice assistant ·
11 overlay, review, consent and on-device privacy · 12 integration and field validation.

## 4. Metrics
Session survival rate; bundle validation pass rate; upload completion rate; keyframe sharpness; coverage of the
room shell at finish; missing-area recall against hand labels; instruction follow rate, prompts per minute,
repetition rate; urgent-line latency (< ~0.5 s); reconstruction success guided vs unguided (measured with Part 2);
battery and temperature per minute; supported devices.

## 5. Out of scope for Part 1
Reconstruction, dense depth, meshes, splats, the full semantic room model, measurements, floor plans, viewers,
accounts/billing/job orchestration (all Part 2). Ask-the-Video, chat bots and Task Packs (later products).
