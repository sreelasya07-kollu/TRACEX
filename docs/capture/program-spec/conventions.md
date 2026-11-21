# CaptureKit conventions (frozen)

These conventions apply to the C++ core, Scan Bundles, `ckit`, and every downstream part. Conversion happens **once at the SDK edge**, using the adapters in `capturekit/core/geometry` (Phase 2). Any change needs an ADR in `capturekit/docs/decisions/`.

## Units and frames
| Item | Convention |
|---|---|
| Length | metres |
| Angle | radians in data; degrees allowed only in config files and human text |
| World frame `W` | right-handed, **Y up** (opposite gravity), origin = AR session origin, X/Z yaw arbitrary but fixed per session |
| Camera frame `C` | **OpenCV**: +X right, +Y down, +Z forward (optical axis), right-handed, attached to the *stored image* orientation |
| Device/IMU frame `D` | platform sensor frame. Gyro and accel are stored **as delivered** (rad/s, m/s²) with `frame` = `ios_device` or `android_device` (see table) |
| Pose | `T_world_from_camera`: maps a point in `C` to `W`, so `p_W = R p_C + t`. Stored as translation `t` + unit quaternion `q` |
| Quaternion on disk | `[w, x, y, z]`, normalised, `w >= 0` (canonical sign) |
| Quaternion in Eigen | `Eigen::Quaterniond(w, x, y, z)` constructor. **`coeffs()` returns `[x, y, z, w]`.** Never serialise `coeffs()` directly; use `geometry::quat_to_wxyz`/`quat_from_wxyz` |
| Bare 4x4 matrices | never part of a public API. Use frame-named types (`Pose<Frame::World, Frame::Camera>`) or names such as `T_world_from_camera` |

## Platform mapping
| Source | Camera axes | World | What the API gives | Conversion to ours |
|---|---|---|---|---|
| ARKit `ARCamera.transform` | X right, Y up, Z backward (looks down −Z), relative to the sensor's native landscape orientation | Y up, gravity aligned (`.gravity` alignment) | world_from_camera, column-major `simd_float4x4` | `T_W_C = T_W_Carkit · F`, F = diag(1,−1,−1) (axis_flip) |
| ARCore `Camera.getPose()` (sensor-oriented) | X right, Y up, Z backward, sensor native orientation | Y up, gravity aligned | world_from_camera, `Pose` (tx,ty,tz, qx,qy,qz,qw) | same as ARKit. **Do not use `getDisplayOrientedPose()` for images.** Note ARCore quaternion order is x,y,z,w |
| OpenGL | X right, Y up, looks down −Z | — | view matrix = camera_from_world | `to_opengl`: `V = (T_W_C · F)^-1`, projection built from intrinsics |
| OpenCV | X right, Y down, Z forward | — | `rvec,tvec` = camera_from_world | identical camera axes; invert the pose |
| COLMAP | OpenCV axes | any | `images.txt`: `cam_from_world`, quaternion `QW QX QY QZ`; pixel centre of top-left pixel at (0.5, 0.5) | `to_colmap`: invert the pose, `cx+0.5`, `cy+0.5`. pycolmap `Rotation3d.quat` is **x,y,z,w**: use `ckit.geometry` helpers |

## Images
- Stored as JPEG in **sensor native orientation** (what the camera delivers: landscape on almost all phones). No pixel rotation and no EXIF Orientation tag (writers must strip it, readers must ignore it).
- `camera.json.orientation.display_rotation_deg` ∈ {0, 90, 180, 270} records how the UI showed the frame. It is metadata only.
- If a consumer rotates pixels, it must use `geometry::rotate_image`, which updates intrinsics *and* pose together (90° CW: `W'=H`, `H'=W`, `fx'=fy`, `fy'=fx`, `cx'=H−1−cy`, `cy'=cx`, `R_C_C' = Rz(−90°)`).

## Intrinsics
- Model `PINHOLE` (fx, fy, cx, cy) or `OPENCV` (+ k1, k2, p1, p2), in pixels of the **stored image** resolution.
- Pixel convention: OpenCV, so the centre of the top-left pixel is (0, 0) and `u = fx·X/Z + cx`.
- ARKit `ARCamera.intrinsics` refers to `capturedImage` resolution. ARCore `Camera.getImageIntrinsics()` refers to the CPU image. If the JPEG is captured at another resolution (high-res still), apply `scale_intrinsics`, and `crop_intrinsics` if the aspect ratio changes.
- Assumption: platform intrinsics already use the integer-centre convention (ADR 0003). Verified per device by checkerboard calibration (ADR 0005); the reprojection report alone is too noisy to detect ±0.5 px.

## Time
- One canonical clock: `t_ns`, int64 nanoseconds, monotonic, mapped once at the edge.
- iOS: `t_ns = round(timestamp · 1e9)` for `ARFrame.timestamp` and `CMLogItem.timestamp` (both seconds since boot on the uptime clock, which does not advance during sleep).
- Android: base = `SystemClock.elapsedRealtimeNanos()`. `SensorEvent.timestamp` and ARCore `Frame.getTimestamp()` are assumed to share it, but this must be verified per device with the clock-check screen. Measured offsets go to `clock-notes.md` and are applied at the edge.
- The core never reads a system clock. Durations come from differences of `t_ns`.

## Depth, tracking and semantics
- Depth is an optional topic (`/depth/index`). Nothing may require it.
- `tracking_state` ∈ {NORMAL, LIMITED, NOT_AVAILABLE}. `tracking_reason` ∈ {NONE, INITIALIZING, EXCESSIVE_MOTION, INSUFFICIENT_FEATURES, INSUFFICIENT_LIGHT, RELOCALIZING, BAD_STATE, CAMERA_UNAVAILABLE, OTHER}. ARKit and ARCore reasons map onto these at the edge.
