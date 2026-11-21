# Scan Bundle v0.1 (proposal from the backend side)

The CaptureKit spec does not exist yet. This is the contract the backend (Parts 3 and 4) can
consume today, with an executable validator. Treat it as a proposal to be agreed with the
capture app team. Code: `formats/scanbundle.py` (models + validator), `spatial/io/scanbundle.py`
(adapter), `eval/bundles.py` (reference writer + broken-bundle corpus). Schema:
`spec/scanbundle/manifest.schema.json`.

## Layout

```
bundle/
  manifest.json
  images/<id>.jpg           undistorted or raw colour frames (JPEG), all the same size
  depth/<id>.png            optional, 16-bit PNG, millimetres, 0 = invalid, SAME SIZE as the image
                            (depth must already be aligned/resampled to the colour image)
```

## manifest.json

| Field | Meaning |
|---|---|
| `schema_version` | `"0.1.0"`; the major version must match (0.x) |
| `capture_id`, `created` | opaque id; ISO 8601 timestamp |
| `device` | `{model, os, app_version}` |
| `tier` | `A` depth + device poses · `B` device poses only · `C` RGB only |
| `units` | `"metres"` |
| `coordinates` | `world_up`: `"+Y"` (ARKit) or `"+Z"`; `camera`: `"opencv"` (x right, y down, z forward) or `"arkit"`/`"opengl"` (x right, y up, z backward) |
| `gravity_world` | unit vector pointing **down**, in the pose world frame (required for tier A/B) |
| `gravity_source`, `pose_source` | `arkit`/`imu_accel`/`none` · `arkit`/`arcore`/`none` |
| `intrinsics` | `fx, fy, cx, cy` (px), `width, height`, optional `distortion {model: "opencv", coeffs: [k1,k2,p1,p2(,k3)]}` |
| `frames[]` | `id, timestamp_ns (int64, strictly increasing), image, depth?, depth_confidence?, T_world_camera? (4x4 rigid, camera→world), tracking_state, exposure_s?, iso?` |
| `consent` | `scan_processing` (must be true), `training_use` (default false), `faces_blurred` |
| `quality_report` | optional free-form on-device report (coverage map, blur scores, user overrides) |
| `files` | `{relative path: sha256}` for every file except `manifest.json` |

Not in v0.1 (needed later): IMU stream, ARKit mesh anchors / RoomPlan JSON, float32 depth,
per-pixel confidence semantics, lens-distortion per frame, GPS/compass.

## Tier rules

| Tier | Poses | Depth | Gravity | Scale |
|---|---|---|---|---|
| A | required | on ≥ 90 % of frames | required | metric from device + depth |
| B | required (≥ 90 % of frames) | optional | required | metric from device poses; depth via OpenMVS |
| C | absent (`pose_source: none`) | absent | optional | **not metric**; room outputs refused |

## Validator (`python -m spatial.cli validate-bundle --bundle <dir>`; exit 1 on errors)

Errors (bundle is rejected): `E_NO_MANIFEST`, `E_SCHEMA`, `E_VERSION`, `E_CONSENT`,
`E_FILE_MISSING`, `E_CHECKSUM`, `E_FILE_NOT_LISTED`, `E_DUP_ID`, `E_FRAME_COUNT` (< 20),
`E_SIZE`, `E_DEPTH_FORMAT`, `E_IMAGE_UNREADABLE`, `E_TIMESTAMPS`, `E_TIER`, `E_POSES_MISSING`,
`E_GRAVITY`, `E_POSE_NAN`, `E_POSE_TELEPORT` (> 8 m/s), `E_POSE_JUMP` (> 90° between frames).
Warnings: `W_PRIVACY`, `W_UNLISTED_FILE`, `W_INTRINSICS`, `W_FRAME_GAP` (> 1 s),
`W_GRAVITY_AXIS`, `W_TRACKING` (> 20 % limited tracking).
Image/depth sizes are checked on a sample of frames (headers only) to keep validation fast.

## Using a bundle

```sh
python -m spatial.cli validate-bundle --bundle <dir>
python -m spatial.cli run --dataset scanbundle --src <dir> --prior device --keyframes --depth-scale
```
`--prior device` turns the device poses into *priors* (never ground truth); the adapter converts
ARKit/OpenGL camera axes to OpenCV exactly (`tests/test_scanbundle.py`). Real bundles produce no
`gt/` folder. Tier C uses `--prior none`.

## Test corpus

`eval/bundles.py` builds a valid sample per tier from the synthetic room and 16 intentionally
broken variants, each with the issue codes it must raise (`BROKEN`). The corpus is generated on
demand (nothing binary is committed).
