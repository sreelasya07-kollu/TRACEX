# Phase 12: Integration and field validation
**Goal/why:** unit tests don't prove people can scan a room. Measure whether real users produce bundles that reconstruct well.

## Decisions
- Success = downstream reconstruction succeeds with low reprojection error and good coverage. Rejected: "didn't crash", subjective ease alone.
- A/B guided vs unguided scans. Rejected: no baseline.
- Review and fix after every 3–5 participants. Rejected: fixing only after all trials.

## Build
1. `ckit e2e`: validate, reconstruct (pycolmap), registered-image ratio, mean reprojection error, end-of-scan room-shell coverage, missing-area recall vs labels, duration, instruction count, voice intents used, retake indicators; JSON + one-page HTML report.
2. `ckit trial-report`: aggregate reports by participant, device, room, guided/unguided, with summary stats and plots.
3. docs/trial-protocol.md (script, consent placeholder, rooms and devices, A/B design, rolling-fix rule); docs/perf-budget.md (sustained FPS, per-frame latency, thermal state at 5 and 10 min, battery per scan, memory, bundle size, per-device table).
4. CI gates: golden replay regressions, schema compatibility (old bundles still validate), upload smoke test, app size budget.
5. docs/release-checklist.md: crash-free target, privacy review items (consent, redaction), device support matrix with known issues, handover notes for Part 2 (bundle contract, RescanRequest).

## Constraints
Change algorithm behaviour only to fix defects found by the harness, each with a regression test.

## Acceptance
e2e and trial-report run on the golden dataset. Pre-agreed targets met, or gaps documented with owners. Perf and trial docs complete. CI gates active.
