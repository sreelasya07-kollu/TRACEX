# Recording protocol (golden sessions)

Raw recordings come from the RawRecorder in each sample app (Phase 0). They are converted with `ckit import-raw` (Phase 1) and replayed by `ckit replay` (Phase 3).

## Raw session layout
```
<session>/
  device.json        platform, model, os, app version, pose_convention (arkit|arcore|opencv), clock base, offsets
  poses.jsonl        {"t_ns", "p":[x,y,z], "q":[w,x,y,z], "tracking", "reason"}   one line per AR frame
  imu.jsonl          {"t_ns", "type":"gyro"|"accel", "v":[x,y,z]}             sensor timestamps, mapped
  intrinsics.jsonl   {"t_ns", "w","h","fx","fy","cx","cy"}                   one line per change (at least the first)
  frames.jsonl       {"i", "t_ns", "file", "exposure_ns"}                    one line per JPEG
  frames/NNNNNN.jpg  JPEG keyframes, about every 0.5 s, sensor orientation
  planes.jsonl       optional AR plane anchors {"t_ns","id","kind","center":[..],"normal":[..],"extent":[w,h]}
  points.jsonl       optional AR feature points {"t_ns","pts":[[x,y,z],...]}
```

## Golden scenarios
| ID | Scenario | Instructions | Duration |
|---|---|---|---|
| G1 | small room (≤12 m²) | walk the perimeter facing the walls, then one pass facing the centre | 60–90 s |
| G2 | large room (≥30 m²) | perimeter plus a centre cross | 120–180 s |
| G3 | corridor | walk to the end and back, sweeping both walls | 60 s |
| G4 | cluttered room | furniture blocking at least 30 % of the floor; scan behind objects | 90 s |
| G5 | low light | one lamp only, < 50 lux at the walls | 60 s |
| G6 | blank white walls | minimal texture; record what tracking does | 60 s |
| G7 | fast motion | normal path at about 2× speed, with quick pans | 45 s |
| G8 | deliberate tracking loss | cover the lens for about 3 s twice, then continue | 60 s |

## Naming
`<scenario>_<device-slug>_<yyyymmdd>_<nn>`, for example `G1_pixel8_20261114_01`. Session folders are never renamed after recording. Real recordings stay out of git (Git LFS `bench/golden/` or object storage).

## Device matrix (fill in per device)
| device-slug | platform | model | OS | ARCore/ARKit ver | depth API | clock offset (ns) | sessions |
|---|---|---|---|---|---|---|---|
| | | | | | | | |

## Synthetic stand-ins
Until device recordings exist, `ckit synth --scenario G1..G8` renders deterministic synthetic sessions in the same raw layout (ADR 0002). Synthetic sessions do not count towards the device-acceptance items.
