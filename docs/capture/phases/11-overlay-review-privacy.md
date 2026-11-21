# Phase 11: Coverage overlay, review/rescan UI, consent and on-device privacy
**Goal/why:** users need to see what is covered and what is missing, review before finishing, and trust that their home is handled safely. Part 1 must not upload faces or screens unredacted.
Read docs/program-spec/part1-room-capture.md §2.5–2.7.

## Decisions
- Overlay data comes from the core (coverage cells, gaps, targets); the shells only draw it (RealityKit/SceneKit on iOS, ARCore + OpenGL/Filament on Android). Rejected: platform-specific coverage logic.
- Review before finalize: summary + weak-area list; tap a gap → guided return to it. Rejected: finish without review.
- Redact faces and screens on device before upload, view-consistently (same region across frames), with a redaction manifest in the bundle. Rejected: server-side redaction only.
- Consent is versioned and stored in the bundle manifest. Rejected: app-level flag only.

## Tests first
- Overlay model: core → draw-list (cells, arrows, progress) deterministic for a replay; golden JSON snapshots.
- Redaction: synthetic frames with faces/screens → masks applied, manifest lists them, the same 3D region stays masked across views; validator rule for missing manifest when redaction is on.
- Consent: bundle without consent version fails validation at the semantic level.

## Build
1. Core OverlayModel (heatmap cells, gap highlights, target arrow, progress ring) + C API accessors.
2. iOS and Android overlay renderers; accessibility: audio/haptic-only mode.
3. Review screen: coverage %, gaps by landmark, thumbnails of weak areas, tap-to-rescan, accept with warnings; shows a RescanRequest from Part 2 when one arrives.
4. Onboarding and permissions (camera, motion, microphone with reasons, safety tips), consent screens with version.
5. On-device redactor (face + screen detector, licence-checked, behind governor) + redaction manifest + validator rules.
6. Telemetry and crash reporting wiring (Sentry) with redacted logs.

## Constraints
Overlay never blocks AR callbacks; draw at display rate from a snapshot. Redaction runs on keyframes before they are finalized.

## Acceptance
Tests pass; Android sample app builds with overlay and review screens; iOS sources written (compile blocked on macOS). Redaction recall measured on a labelled set.
