# Phase 3: Replay harness
**Goal/why:** replaying recorded sessions through the real core makes algorithm work deterministic and doubles as the regression suite.

## Decisions
- Replay real recordings. Rejected: re-scanning by hand (slow, irreproducible). Synthetic rooms deferred (ground truth later, unrealistic noise now).
- Python driver calling the real C++ core via nanobind. Rejected: a Python reimplementation (tests a copy).
- Real-time and fast modes with an injectable clock.

## Build
1. ReplaySink interface (on_pose, on_imu, on_intrinsics, on_frame, on_event, on_end). Audit the core for system-clock use and fix it.
2. `ckit replay`: merge channels ordered by t_ns (documented tie-breaks), --realtime / --fast, small LRU frame cache.
3. Fault injection: --drop-frames, --jitter-ms, --inject-tracking-loss T0:T1, --inject-thermal T:STATE, fixed --seed.
4. Diff-friendly JSON metrics report; `ckit replay-compare` with tolerances.
5. Simple matplotlib trajectory viewer. CI job replaying 3 small golden sessions against committed baselines in under 3 minutes.

## Acceptance
Two identical runs give byte-identical reports. Fault injection produces the expected events. docs/replay.md explains adding a sink and refreshing baselines deliberately.
