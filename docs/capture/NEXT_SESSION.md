# Starting a new session (copy-paste kit)

## 1. Paste this as the first message of the new conversation

```
Continue Part 1 on branch main. Read AGENTS.md, PROGRESS.md and the brief named in its Next
section (under .claude/agent-memory/planner/briefs/) only. Work one batch of
unattended-queue.md at a time, using the repo rules in AGENTS.md. Skip anything that
needs phones, a Mac or people and list it under "blocked" in docs/STATUS.md. Run each
acceptance check for real before reporting. Commit only when asked. Do not push.
```

To work on a specific phase instead, say "Do Phase N" and read `docs/phases/NN-*.md`.

## 2. State at hand-off
- The program is two parts (docs/program-spec/README.md). This whole repository is Part 1 only; Part 2 is the separate repo `roomscan-recon`.
- Branch `main` at `b3d39f0` plus a large UNCOMMITTED tree (~130 files: unattended queue U1-U9 minus U5-C/U7-C). Review and commit it first, only when the user asks. Phase hashes in PROGRESS.md.
- Done: Phases 0–8 in software. Phase 9 mostly (detections channel schema 1.1.0, shell, gaps, landmarks, DetectorWorker flag off); Phases 10–11 Android (voice, consent schema 1.2.0, upload UI, Sentry field-only, research log). Not done, in order: U5-C, U7-C (ADR 0023), U10-C, U11-CT (1.3.0), U11-A, real U12-D; iOS blocked. Regression at stop: core ctest 163/163, WSL pytest 126 passed/8 skipped, ruff clean (2 files reformatted after the run), Android 184/184 + both flavours OK.
- Tests only the human can run: `docs/HUMAN_TESTS.md`.
- Full status and the list of things only the user can do: `docs/STATUS.md`.
- Gotchas and working commands: `PROGRESS.md`.

## 3. One-time machine setup (already done on the dev machine; repeat on a new one)
```sh
cd capturekit
git submodule update --init --recursive
./scripts/bootstrap.sh                       # tools/.venv, OpenCV prebuilt, references
cd tools && uv sync --extra dev && cd ..
scripts/build_core_py.sh                     # builds _ckcore (needs cmake, g++, python dev headers)
scripts/make_golden.sh                       # synthetic golden sessions, about 20 min, in build/golden/raw
# Android (optional): install the SDK (cmdline-tools, platform 37, build-tools 37.0.0), then
export ANDROID_HOME=$HOME/Android/Sdk
```
Tools used: cmake 4.x, g++/clang, protoc, uv, Python 3.12 (via uv), Java 21, docker.

## 4. Check the baseline before starting
```sh
cd capturekit
# Windows has no C++ toolchain: run core ctest and pytest in WSL (recipes in PROGRESS.md Commands and capturekit/scripts/README.md)
cd tools && uv run pytest -q && uv run ruff check . && uv run ruff format --check .      # WSL
cd ../android && ./gradlew --no-daemon -Pkotlin.incremental=false :capturekit:testDebugUnitTest :sample-app:assembleStandardDebug :sample-app:assembleFieldDebug --console=plain   # 184 tests
```
If a test hangs, run it under `ulimit -v 3000000` and `timeout`; see Gotchas in PROGRESS.md.

## 5. Rules that matter most (full text in AGENTS.md)
- Core never reads the system clock; time comes from `t_ns`.
- No bare 4x4 matrices in public APIs; use frame-named types.
- Quaternions on disk are `[w,x,y,z]`; Eigen `coeffs()` is `[x,y,z,w]`.
- Record assumptions as 3–6 line ADRs in `capturekit/docs/decisions/` (next free number: 0025, 0023 reserved for U7-C; index in README.md).
- End each phase by updating `PROGRESS.md` and `docs/STATUS.md`.
- Final report is at most 25 lines.
- Real device recordings go in `capturekit/bench/golden/` (Git LFS). They replace the synthetic sessions and the Phase 1 and 2 checks must then be re-run on them.

## 6. When the user brings real recordings
Say: "Process the golden recordings in capturekit/bench/golden/". Then:
1. `ckit inspect-raw` each session, filling the device matrix in `docs/program-spec/recording-protocol.md`.
2. `ckit inspect-raw <clock-session> --clock` and record the offsets in `docs/program-spec/clock-notes.md`; apply any offset above 1 ms in the recorder or `device.json`.
3. `ckit import-raw` and `ckit validate` each one; fix any recorder bugs found.
4. Re-run `ckit reprojection-report` on the real bundles. Check ADR 0003 with the checkerboard recordings (ADR 0005): calibrated cx, cy vs platform intrinsics, |Δ| ≈ 0.5 px refutes it.
