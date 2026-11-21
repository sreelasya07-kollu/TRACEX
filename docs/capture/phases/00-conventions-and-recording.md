# Phase 0: Freeze conventions and record real data
**Goal/why:** coordinate, clock and unit mistakes are silent and expensive later. Real recordings are needed to tune every later phase.

## Decisions
- One internal convention converted at the SDK edge. Rejected: ARKit's or COLMAP's convention everywhere (conversions scatter, or guidance suffers).
- One monotonic ns clock. Rejected: wall clock (NTP jumps), per-sensor clocks (can't correlate IMU with frames).
- Throwaway raw recorders first. Rejected: full app first (format ossifies by accident), specs only (no real data for weeks).

## Build
1. docs/program-spec/conventions.md: everything in AGENTS.md §C + a table mapping ARKit, ARCore, OpenGL, OpenCV, COLMAP to ours; image storage orientation; intrinsics semantics.
2. RawRecorder in both sample apps writing a session folder: poses.jsonl (t_ns, pose, tracking state + reason), imu.jsonl (gyro, accel, sensor timestamps), intrinsics.jsonl, frames/ (JPEG keyframes ~every 0.5 s), device.json. No UI polish, guidance or upload.
3. Clock-check screen logging raw AR-frame and IMU timestamps beside the platform monotonic clock → docs/program-spec/clock-notes.md.
4. `ckit inspect-raw`: top-down trajectory plot, IMU rates, dropped-frame counts.
5. docs/program-spec/recording-protocol.md: 8 golden scenarios (small room, large room, corridor, cluttered, low light, blank white walls, fast motion, deliberate tracking loss), naming scheme, device-matrix template.

## Acceptance
Recorders build in CI. ≥8 golden sessions exist, each plotted by inspect-raw. Clock offsets documented per device. conventions.md has no TODOs. A small session is kept in tests/fixtures.
