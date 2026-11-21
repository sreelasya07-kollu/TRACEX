# Phase 2: Geometry core
**Goal/why:** geometry bugs fail quietly (reconstruction looks nearly right, then doesn't converge). Prove the maths first.

## Decisions
- Small named conversion steps composed in one place. Rejected: hand-written matrix products at call sites, one opaque convert(from, to).
- Test by projection invariance + pycolmap reprojection. Rejected: comparing only to hard-coded matrices (encodes the author's assumptions).
- SLERP + lerp interpolation, refusing to extrapolate. Rejected: linear matrix interpolation.

## Tests first
- Random 3D points and poses projected with each convention's own maths (ARKit/ARCore, OpenGL, OpenCV/COLMAP). After converting pose and intrinsics, pixels match within 1e-6 px. Cover 0/90/180/270° image rotations and resolution scaling.
- Quaternion order tests. Interpolation tests (endpoints, midpoint angle, refuses beyond max gap).

## Build
1. Types: Pose (Sophus SE3d with explicit frame names), Intrinsics, CameraModel.
2. Steps: axis_flip (diag(1,-1,-1)), invert, rotate_image (swaps fx/fy, remaps cx/cy, rotates the pose about camera Z), scale_intrinsics, crop_intrinsics. Compose once per adapter: from_arkit, from_arcore (use the sensor-oriented pose for images), to_colmap, to_opengl.
3. Time alignment: interpolation and nearest lookup with a tolerance.
4. nanobind bindings; `ckit export-colmap` (cameras, images with world-to-camera wxyz, empty points). A script reporting pycolmap reprojection error for every golden bundle.

## Acceptance
All tests pass. Reprojection report committed to docs/reports/ showing small, stable error across sessions.
