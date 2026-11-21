# Phase 5: Thermal and battery governor
**Goal/why:** throttled phones lose tracking. Degrading gracefully early beats losing tracking mid-room.

## Decisions
- OS signals + self-measured frame time and drop rate. Rejected: fixed conservative settings (waste capability), OS signals only (coarse, inconsistent across Android OEMs).
- Hysteresis (separate exit thresholds) + minimum dwell. Rejected: single thresholds (flapping).
- Pure C++ state machine with thin platform adapters. Rejected: separate Swift and Kotlin logic.

## Tests first
Table-driven scripted timelines: slow heat-up, rapid oscillation at a boundary (must not flap), sudden critical, recovery, low battery charging vs not, dropped-frame spike on a cool device.

## Build
1. Governor input Signals {thermal_level 0..3, low_power_mode, battery_pct, charging, frame_time_ms_ema, dropped_frame_rate, t_ns}; output Policy {keyframe_rate_scale, ml_rate_scale, capture_resolution_tier, overlay_enabled, upload_allowed, must_pause, reason}.
2. Degrade ladder: nominal → reduce keyframe rate → reduce ML/overlay rate → lower resolution tier → warn → safe pause. Config-driven. Every transition is an /events record.
3. iOS adapter (ProcessInfo thermalState + notification, low-power mode, battery). Android adapter (PowerManager thermal status listener API 29+, thermal headroom API 30+ guarded, BatteryManager).
4. Safe pause/resume contract: the session layer flushes and checkpoints on must_pause.
5. ReplaySink support for --inject-thermal. docs/soak-test.md with a 10-minute soak protocol and results template.

## Acceptance
All timeline tests pass. Replay with injected thermal events changes the policy as specified.
