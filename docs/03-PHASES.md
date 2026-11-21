# Phases / Roadmap

**Current phase:** P11 (glue: Scan Bundle, QA gate, router, exports, golden dataset)
**Last updated:** 2026-11-17

> Phases come from the charter's core features. Only the owner changes this file.
> This file owns phase names, charter links and the current phase. It does not own:
> - **status**: [PROGRESS.md](../PROGRESS.md) (phase table and evidence sections);
> - **thresholds**: [HANDOFF.md §5](../HANDOFF.md) (PROPOSED values, changed only with a
>   written reason);
> - **measured numbers**: [PHASE_REPORTS/](PHASE_REPORTS/) and `runs/test_results/`.
> Open work per phase is in [04-TASKS.md](04-TASKS.md).

| Phase | Name | Charter core feature | Status | Report |
|---|---|---|---|---|
| P0 | Environment, pinned deps, Docker tools, env tests | constraints (§8) | [PROGRESS.md](../PROGRESS.md) | [PHASE_0](PHASE_REPORTS/PHASE_0.md) |
| P1 | Ingest, adapters, keyframes | #1 | [PROGRESS.md](../PROGRESS.md) | [PHASE_1](PHASE_REPORTS/PHASE_1.md) |
| P2 | Pose refinement | #2 | [PROGRESS.md](../PROGRESS.md) | [PHASE_2](PHASE_REPORTS/PHASE_2.md) |
| P3 | Depth + scale | #3 | [PROGRESS.md](../PROGRESS.md) | [PHASE_3](PHASE_REPORTS/PHASE_3.md) |
| P4 | Fusion | #4 | [PROGRESS.md](../PROGRESS.md) | [PHASE_4](PHASE_REPORTS/PHASE_4.md) |
| P5 | Planes + room shell | #5 | [PROGRESS.md](../PROGRESS.md) | [PHASE_5](PHASE_REPORTS/PHASE_5.md) |
| P6 | SpatialMap writer, schemas, CLI | #6 | [PROGRESS.md](../PROGRESS.md) | [PHASE_6](PHASE_REPORTS/PHASE_6.md) |
| P7 | Part 4 sparse | #7 | [PROGRESS.md](../PROGRESS.md) | [PHASE_7](PHASE_REPORTS/PHASE_7.md) |
| P8 | Dense + mesh + texture | #7 | [PROGRESS.md](../PROGRESS.md) | [PHASE_8](PHASE_REPORTS/PHASE_8.md) |
| P9 | Gaussian splatting (GPU) | #8 | [PROGRESS.md](../PROGRESS.md) | [PHASE_9](PHASE_REPORTS/PHASE_9.md) |
| P10 | Reconstruction packaging + end-to-end | #9 | [PROGRESS.md](../PROGRESS.md) | [PHASE_10](PHASE_REPORTS/PHASE_10.md) |
| P11 | Glue: Scan Bundle, QA, router, exports, golden dataset | #1 (bundle), #10-#13 | not yet tracked in PROGRESS.md (T-008) | none yet |

---

## P0: Environment
**Source:** HANDOFF.md §3 (E1-E8). **Exit criteria:** every E-test in HANDOFF.md §3 passes on
the machine that can run it; GPU-only tests run in the GPU batch (T-024).

## P1-P10
**Source and thresholds:** the row for the phase in [HANDOFF.md §5](../HANDOFF.md).
**Exit criteria (same for each phase, from docs/PLAN.md §5 and HANDOFF.md §2 rule 10):**
- [ ] Every acceptance line of the phase in HANDOFF.md §5 has a measured number in
  `runs/test_results/` and in the phase report.
- [ ] Any threshold change has a written reason in the phase report.
- [ ] Open items for the phase in 04-TASKS are DONE or explicitly deferred by the owner.

## P11: Glue (current)
**Source:** docs/PLAN.md §4 items 1-5 (item 6, Semantics v2, is a charter non-goal).
**Charter link:** core features #1 (Scan Bundle adapter + validator), #10 QA gate,
#11 router, #12 exports, #13 golden dataset.
**Exit criteria**
- [ ] Each of PLAN §4 items 1-5 has code, a measured test and an entry in PROGRESS.md.
- [ ] Format changes documented in INTERFACES.md / SCAN_BUNDLE.md with schemas in `spec/`.
- [ ] The P11 rows in 04-TASKS are DONE or deferred by the owner.
