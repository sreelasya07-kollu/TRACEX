# Device test log

Results of tests run on a real phone. Each row names the docs/HUMAN_TESTS.md item it covers.
Evidence (screenshots, pulled bundles, logcat) lives in `capturekit/build/device/` (not committed).

## 2026-11-17 — CPH2381 (OPPO), Android 14 / API 34, arm64-v8a, standard debug build, run over adb

Build: uncommitted tree on b3d39f0 (waves U1–U9, FIX, U5-C). Tests were driven by adb taps; nobody walked the room.

| Test | HUMAN_TESTS | Result | Notes |
|---|---|---|---|
| Start without consent opens the consent screen | H13 | PASS | `ConsentActivity` opens; "Placeholder text – pending legal owner" banner shown |
| Accept disabled until the text is scrolled to the end | H13 | PASS | enabled after reaching the end; 8 scroll swipes never record consent |
| Optional toggles default off | H13 / H28 | PASS | upload + anonymous reports both off |
| Decline | H13 | PASS | back to Home, nothing saved |
| Accept continues straight into the scan | H13 | PASS | prefs: version 1, UTC ISO time, upload/telemetry false |
| Withdraw consent (confirm dialog) | H13 | PASS (retest by hand) | first tries missed (taps during dialog animation / after rotation); a later tap cleared consent |
| Bundle carries consent | H13 | PASS | manifest schema 1.2.0, `consent {version 1, at}`, `privacy {redaction_mode: faces}` |
| Scan start, Pause/Resume | H9 | PASS | timer holds while paused (00:32), resumes |
| HUD | H9 | PASS (partial) | REC timer, status pill ("More light"), instruction, room checklist, mic chip with full content description |
| Home mid-scan, return | H5 (R-home) | PASS | returns paused with Resume; no crash |
| Back on the capture screen while recording | H5 | PASS | bundle finalized and saved (scan_212703, 10 keyframes) |
| Force-stop mid-scan, relaunch | H4 / H5 | PASS (bug B2) | "Last scan ended unexpectedly and was recovered"; consent kept in the recovered manifest |
| My scans list | H29 | PASS | date, verdict badge, size, folder; rows have TalkBack descriptions |
| Crashes / ANRs during all of the above | H5 | PASS | 0 FATAL / SIGSEGV / ANR in logcat |
| Dark room behaviour | H8 | PASS | ARCore `INSUFFICIENT_LIGHT` for the whole scan; HUD showed "More light"; 0 keyframes saved |

## Bugs found (being fixed in the DEV-A / DEV-CT tasks)
- B1 **t_ns = 0 pose**: scan_20261117_212703 starts with a /pose and /intrinsics at time 0 (rest at ~4.84e14 ns), giving a fake 5.6-day POSE_GAP. Seen when the scan starts right after the consent screen. Fix: drop frames with timestamp 0 in ARAdapter; core rejects t_ns <= 0; validator ERROR TIME_ZERO.
- B2 **Recovered bundle fails validation**: after force-kill, `privacy/redaction.json` is missing while `redaction_mode = faces` → ERROR REDACTION_MANIFEST_MISSING. Fix: write the file at session start; recover() writes an empty one if missing.
- B3 **0 keyframes is only a WARN**: a bundle with no keyframes is unusable but shows "Usable, with warnings". Fix: ERROR NO_KEYFRAMES.
- B4 **Review screen in landscape**: no action buttons visible (no Done / My scans / New scan), path clipped, no scroll.
- B5 **No review sheet when nothing was captured**: Finish with no floor and no walls saves directly.
- B6 (minor) POSE_GAP warns on gaps that are only a user Pause. Fix: ignore paused intervals.

## 2026-11-17 (later) — first walked scan, lights on, build with the DEV-A fixes
Bundle scan_20261117_215819: 4:17, 118 keyframes, 16.6 MB, `ckit validate --level quality` PASS (0 errors). Tracking 100 %, ~46k AR points, no t=0 timestamps, privacy/redaction.json present, 0 crashes.
- DEV-A fixes verified on device: Review screen in landscape now shows My scans / Home / New scan; B1 and B2 not reproduced.
- **FAIL (guidance), H9/H22**: user stopped the scan by hand after 4+ min; it never completed and sent the user over the same area 3+ times. Guidance log: completeness stayed 3–7 %, coverage rose to 0.42 then fell to ~0.25; 69 instructions in 257 s (~16/min, budget 8/min), 39 turn prompts, 42 superseded, no tilt-up prompt, ceiling never found; checklist showed 8 walls ("6 done, 1 scanning, 1 just found") in a ~4-wall room. User also reported the on-screen arrow did not look right. Under diagnosis by replaying this bundle (brief real-walk-guidance-fix).
- Phone and ARCore are not the cause: the recorded data is complete and valid. The object detector is off by default and played no part.

## Still needs a person with the phone
Since issue #6 (ADR 0032) the default build hides every feature not listed as PASS above; docs/STATUS.md section 0 has the table. First: a walked scan of the default build (only kept features visible, no crash, `ckit validate` PASS on the pulled bundle) — pending.
Switched off until they pass (enable with `adb shell setprop debug.capturekit.<switch> 1` on a debug build, force-stop, then test): guidance `guidance` (H9 walls filling, H10 review sheet with a weak wall, H11 mini map, H22), voice commands `voice_commands` (H12, API 34 supports them; check the offline English pack; first-time microphone prompt needs `adb shell pm revoke dev.capturekit.sample android.permission.RECORD_AUDIO` first), detector `detector` (H18–H21, H33), recalibration card `recalibrate_card` (H36 cover/uncover).
Still ON but unverified: face redaction with a consenting person (H14). Field build only: upload with the dev server (H16, H17).
