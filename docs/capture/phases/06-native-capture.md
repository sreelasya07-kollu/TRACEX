# Phase 6: Native capture pipelines (iOS and Android)
**Goal/why:** real data enters here. Failures are operational (blocked callbacks, lost frames, corrupt sessions on crash, device quirks), so design for robustness.
Parallelism: iOS and Android can run as separate sessions, each with this file + the C API header only.

## Decisions
- Native shells over shared C++ through a small stable C API. Rejected: cross-platform UI, duplicated apps, a wide C++ binding surface, Swift C++ interop/SWIG/JNA (toolchain fragility).
- ARKit and ARCore for tracking. Rejected: own SLAM, heavy open-source SLAM.
- High-quality JPEG keyframes. Rejected: HEIC (support varies), HEVC segments (hard frame-exact extraction).

## Build
1. C API in capturekit/core/capi/: ck_session_create/start/pause/resume/finalize, ck_ingest_pose/imu/intrinsics/frame, ck_poll_events, ck_policy_get. Session state machine (idle, calibrating, scanning, paused, finalizing, complete, failed) with tests for legal and illegal transitions. Quality and governor wired in behind it.
2. iOS: ARAdapter (ARWorldTrackingConfiguration, gravity alignment; forward pose, intrinsics, tracking state, camera image; high-resolution frame capture for keyframes where supported), Sensors (CMMotionManager at the highest sensible rate), Session + GRDB frame store (image file → DB row → periodic checkpoint), interruption and permission handling, SampleApp screen with live quality and policy.
3. Android: ARAdapter (ARCore Session, sensor-oriented pose, Camera.getImageIntrinsics), Sensors (SensorManager fastest rate, event timestamps), Room frame index, WorkManager, optional foreground service. Prototype first whether full-resolution keyframes need a Camera2 still capture alongside ARCore; record the finding in an ADR.
4. Both: bounded queues, drops logged, encoding on workers, timestamps mapped to the canonical clock per Phase 0 notes, crash recovery turns an interrupted session into a valid partial bundle.
5. docs/capture-test-protocol.md: 5-minute scan, forced kill, call interruption, backgrounding, tracking loss, low light.

## Constraints
No business logic in Swift/Kotlin that exists in core. No network code in this phase.

## Acceptance
On ≥2 iOS and ≥3 Android devices of different tiers (listed in docs/reports/devices.md): a 5-minute scan validates with 0 ERROR and reconstructs in pycolmap; forced-kill recovery works; CI builds all targets.
