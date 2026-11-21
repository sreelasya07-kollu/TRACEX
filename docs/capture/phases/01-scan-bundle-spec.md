# Phase 1: Scan Bundle spec and serialization
**Goal/why:** the bundle is the contract between capture and everything downstream. The validator is the executable definition of "valid".

## Decisions
- MCAP + separate JPEGs. Rejected: JSON/CSV folder (slow, not crash-safe), video + pose sidecar (frame-exact extraction and per-frame intrinsics are painful), SQLite (poor for append-only streams and replay).
- Protobuf channels. Rejected: JSON at IMU rates.
- Validator severities ERROR / WARN / INFO. Rejected: reject-on-anything (users lose good scans), warn-on-everything (garbage passes silently).

## Tests first
Fixtures of valid and broken bundles (truncated MCAP, wrong sha256, keyframe without pose, non-monotonic timestamps, NaN pose, non-unit quaternion, missing intrinsics), plus a pytest suite expecting specific error codes.

## Build
1. capturekit/spec/scanbundle/: JSON Schemas for manifest.json (schema_version, bundle_id, session_id, device, conventions, files[] with sha256 + size, counts, clock info, capture settings, completion status) and camera.json (model, size, intrinsics, orientation). Protobuf for /pose, /imu/gyro, /imu/accel, /intrinsics, /frame_index, /events. SPEC.md with layout and semver rules (reject unknown major, warn on unknown minor).
2. C++ streaming writer (chunked, timed flush, finalize with summary) and a reader that can also scan an unfinalized file sequentially. nanobind bindings.
3. `ckit validate` with levels structural, integrity, semantic (monotonic time, keyframe has pose within tolerance, intrinsics present, no NaN, unit quaternions), quality (stub). Output: JSON report + human summary.
4. `ckit import-raw` to convert Phase 0 sessions.
5. Round-trip tests (C++ write, Python read) and a truncation fuzz test that must never crash.

## Acceptance
All golden sessions import and validate with 0 ERROR. Each broken fixture gives its expected code. Round-trip equality holds within float tolerance.
