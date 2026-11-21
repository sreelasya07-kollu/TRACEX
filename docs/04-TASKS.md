# Task Board

**Rules**
- Every planner brief names one task ID from this board.
- The main session claims a task (IN PROGRESS, owner, date) when the owner OKs its brief, and
  moves it to DONE with the commit hash at commit. Nobody else changes task status.
- Every task links to a phase in [03-PHASES](03-PHASES.md). No phase = not allowed.
- New work you discover becomes a new TODO row; don't silently build it.
- ID format: `T-NNN`, increment from the highest existing ID. IDs are never reused.
- Source conflicts are listed here as TODO rows and resolved by the owner, not by an agent.
- Status of phases lives in [PROGRESS.md](../PROGRESS.md); thresholds in
  [HANDOFF.md §5](../HANDOFF.md). Rows link to them instead of copying numbers.

## IN PROGRESS
| ID | Task | Phase | Owner | Started | Notes |
|---|---|---|---|---|---|
| T-001 | Adopt project docs template (charter draft, 01-05, ADR/amendment/CR/handoff folders, fact map, CLAUDE.md, PLAN banner) | P0 | main session | 2026-11-17 | brief `docs-template-adopt`; slash commands are its main-session step |
| T-003 | Windows backslash manifest keys: keys built with `str(p.relative_to(...))` contain `\` on Windows. Write sites: `spatial/stage.py`, `spatial/spatial_map.py`, `recon/package.py`, `recon/dense.py`, `eval/bundles.py`. Comparing validators: `formats/scanbundle.py`, `formats/spatial_map.py`, `formats/reconstruction.py` | P6, P10, P11 | main session | 2026-11-18 | brief `posix-manifest-keys`; owner away, planner settled questions; coordinator count: 9 sites / 11 failing light tests; recount when fixing |

## TODO (priority order, top = next)
| ID | Task | Phase | Owner | Depends on | Notes |
|---|---|---|---|---|---|
| T-002 | Owner manual steps: `mv docs/charter-draft.md docs/00-PROJECT-CHARTER.md`, remove the DRAFT comment block, set `**Status:** LOCKED` and fill Locked on / Approved by, run `bash scripts/lock-charter.sh`; `git config core.hooksPath .githooks`; `git update-index --chmod=+x` on the hook and charter scripts; commit; push; enable branch protection on `main` (require PR, code-owner review, "Charter integrity" check) | P0 | owner | T-001 | owner only; agents never touch the charter path or push. See `.github/CODEOWNERS` |
| T-004 | `tests/test_ingest_dry_run.py` has no `light` marker, so `pytest -m light` skips it | P1 | | | |
| T-005 | CI runs no project tests: `.github/workflows/charter-check.yml` only runs `verify-charter.sh`. Add `pytest -m light` + `ruff check` | P0 | | T-003, T-004 | needs an ADR update if it changes ADR 002 |
| T-006 | Source conflict: PROGRESS.md phase table row 1 says "Scan Bundle adapter blocked (no spec)", but `docs/SCAN_BUNDLE.md` v0.1, `formats/scanbundle.py`, `spatial/io/scanbundle.py` and `tests/test_scanbundle.py` exist (commit e5860cc) | P1, P11 | owner | | owner decides the correct status; agents do not edit PROGRESS.md for this |
| T-007 | Source conflict: PROGRESS.md P4 and P8 rows point to a "Phase 8 evidence" section that does not exist; `docs/PHASE_REPORTS/PHASE_8.md` shows the ETH3D targets as PENDING | P4, P8 | owner | | owner decides whether numbers exist or the rows are wrong |
| T-008 | Source conflict: P11 work is missing from PROGRESS.md (Scan Bundle, QA gate `qa/`, router `router/`, exports `export/`; commits 30924c3, e5860cc) | P11 | owner | | add a P11 row and evidence, or confirm it stays untracked |
| T-009 | Source conflict: HANDOFF.md §1, §3 describe the old `reconstruction/packages/...` layout and `scripts/fetch_test_data.sh`; the repo is flat (`spatial/`, `recon/`, ...) and fetch scripts are `data/fetch.sh`, `scripts/heavy/fetch_datasets.sh` | P0 | owner | | HANDOFF § numbers are cited by tests and inventory.yaml: do not renumber |
| T-010 | `pyproject.toml` `[tool.hatch.build.targets.wheel] packages` lists spatial, recon, formats, eval but not router, qa, export | P11 | | | found during T-001 |
| T-011 | Router fallback: SfM failed -> feed-forward poses (VGGT / MapAnything, licence-checked) as a GPU worker | P11 | | T-024 | docs/PLAN.md §4 item 3; licence check into MODELS.md; new dependency needs an ADR |
| T-012 | Exports still open: E57 (only LAZ today), USDZ packaging (today `usdzip` outside the repo), splat SPZ / glTF `KHR_gaussian_splatting` | P11 | | | docs/PLAN.md §4 item 4, §3 P9; current state in INTERFACES.md §8 |
| T-013 | Golden dataset + regression gate: tape-measured rooms; every library or model upgrade reruns the benchmark suite and compares F-score, wall offsets, area | P11 | | | docs/PLAN.md §4 item 5 |
| T-014 | P4: ETH3D eval-tool F-score of the office fused cloud (report 1/2/5/10 cm) | P4 | | T-007 | threshold: HANDOFF.md §5 P4; docs/PLAN.md §3 P4 |
| T-015 | P4: `--max-frames` streaming so a 2000-frame scene fuses within the RAM budget in docs/PLAN.md §3 P4 | P4 | | | not recorded in PROGRESS.md |
| T-016 | P3: consistency filter thresholds depend on baseline + test that a real scene keeps enough pixels | P3 | | | docs/PLAN.md §3 P3 item 3d; not recorded in PROGRESS.md |
| T-017 | P5: re-run Replica room0 polygon IoU vs the GT-mesh polygon after the footprint-contour change | P5 | | | heavy test exists; PROGRESS.md P5 known issues; target in docs/PLAN.md §3 P5 |
| T-018 | P5: feed the floor normal back as a gravity frame correction, stored in `room.json` | P5 | | | docs/PLAN.md §3 P5; PROGRESS.md P5 known issues (tilt) |
| T-019 | P5: `scale_confidence` documented heuristic rule (today null) | P5 | | | docs/PLAN.md §3 P5; schema change -> INTERFACES.md |
| T-020 | P5: ETH3D floor-perpendicular-wall check: gravity prior from GT floor (eval only) or drop the criterion with a written reason | P5 | owner | | docs/PLAN.md §3 P5; HANDOFF.md §5 P5; owner decision needed |
| T-021 | P8: measure ETH3D dense and mesh F@5cm and textured held-out PSNR | P8 | | T-007 | thresholds HANDOFF.md §5 P8; PHASE_8.md shows PENDING |
| T-022 | One-command check script (`pytest -m light` + ruff) | P0 | | T-004 | docs/PLAN.md §2 item 4; no `scripts/check.sh` or Makefile exists |
| T-023 | Disk hygiene: free space under `runs/bench_phase3` per docs/PLAN.md §2 item 2 | P0 | owner | | current state not recorded; deleting runs needs owner OK |
| T-024 | GPU batch: run the items in [heavy-compute/INVENTORY.md](heavy-compute/INVENTORY.md) in its priority order (E4/E5, `pytest -m gpu`, `pytest -m arkit` incl. ARKitScenes adapter, CUDA Docker build, P9) | P0, P9 | | GPU machine | docs/PLAN.md §3 "GPU machine batch"; PROGRESS.md "Pending for the GPU test machine" |
| T-025 | E6 env test registers 26/26 and runs: fix `DATA` path (#2) and use known-K resize like `refine` (#1); record measured result | P0 | | | brief e6-eth3d-env-fix; issues #1, #2 |
| T-026 | ETH3D office SfM repeat-run gate: min registered over 5 seeded `refine` runs + split-model warning in SpatialMap quality; measure fixes if min < 26/26 | P2, P11 | | | issue #4; brief `sfm-repeat-gate`; known issue in PROGRESS.md "Phase 2 evidence" |
| T-027 | Measure INTER_AREA pre-resize in ingest (`--max-side`) vs COLMAP internal resize on ETH3D-proxy and Replica; report only, no default change without a Scan Bundle | P2 | | T-026 | issue #3; brief `sfm-preresize-measure` |

## BLOCKED
| ID | Task | Phase | Owner | Blocked by | Notes |
|---|---|---|---|---|---|

## DONE
| ID | Task | Phase | Owner | Completed | PR / commit |
|---|---|---|---|---|---|
