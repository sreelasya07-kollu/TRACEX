# Human tests

The list of tests only the human can run (phone, people, accounts, decisions). Updated by each batch of the unattended queue; tick items as done.

## Setup
Phone: phone CPH2381 with USB debugging, `adb devices` shows it; `adb shell getprop ro.build.version.sdk` (note it; < 31 means no voice commands); install `cd capturekit/android && ./gradlew :sample-app:installStandardDebug` (field build: `-PTUS_URL=http://<LAN-IP>:1080/files/ :sample-app:installFieldDebug`). Bundle path = the "Saved:" line on the Review screen, pull with `adb pull <path>`.

## Tests

### Phase 0-1 recordings (Gate 0)
- [ ] H1 record G1-G8 per docs/program-spec/recording-protocol.md in 10+ real rooms, folder `G<n>_<device>_<yyyymmdd>_<nn>` under capturekit/bench/golden/. Pass: `ckit inspect-raw` and `ckit import-raw` + `ckit validate --level quality` give 0 ERROR for each.
- [ ] H2 clock: `ckit inspect-raw <clock-session> --clock`; pass: offsets recorded in docs/program-spec/clock-notes.md, any > 1 ms applied.
- [ ] H3 checkerboard (ADR 0003/0005): `ckit reprojection-report`; pass: calibrated cx, cy within 0.5 px of platform intrinsics or ADR updated.

### Phase 5-6 capture/soak
- [ ] H4 follow capturekit/docs/capture-test-protocol.md and soak-test.md (10 min). Pass: no crash, thermal level logged, forced-kill then `ckit recover DIR` keeps data.

### Stability (docs/android-perf-baseline.md)
- [ ] H5 R1-R7 x3 (Back/Finish/Home/power/relaunch cases) with `adb logcat -d | tail -n 200` after each. Pass: 0 crashes in 21 runs or each crash logged; fill the "?" cells.
- [ ] H6 frame pacing A/B row, Activity.onPause timing (< 200 ms target as measured), 60 s walk table (keyframes/min, drops < 5 %, GC lines < 1/min).
- [ ] H7 only if crashes persist: allowNativeHeapPointerTagging A/B over 10 runs, then decide to code stability step 8.

### Guidance and UX
- [ ] H8 `adb logcat -s CK/Voice` over 20 prompts; pass: median instruction to TTS start < 500 ms.
- [ ] H9 walk 60 s: screenshots of HUD (no overlap at 59 s), RoomView walls filling and the yaw ring moving in a plain room, checklist read by TalkBack ("Walls: 2 done, 1 scanning"), finish glow + haptic, wall tick haptic at 80 %.
- [ ] H10 Review sheet screenshot with a weak wall; "Scan the gaps" resumes the same bundle; "Finish anyway" finalizes.
- [ ] H11 mini map moves as you walk (no lag > 1 s).

### Voice (needs API 31+ and an on-device speech pack)
- [ ] H12 airplane mode on, 20 spoken commands (pause, resume, repeat, what is missing, finish + confirm, louder, quieter); pass: median speech end to action < 1 s in `adb logcat -s CK/Voice`, mic hidden with a plain message when no pack; permission rationale shows before the first RECORD_AUDIO prompt.

### Privacy and consent
- [ ] H13 first launch shows consent; Start blocked until accepted; revoke in Home works; new bundle manifest has consent.version (`ckit validate <bundle>` OK).
- [ ] H14 redaction: 60 s walk with a consenting person in view; pull bundle, open 10 keyframes; pass: faces blurred, privacy/redaction.json present, keyframes/min within 10 % of baseline; record recall on a labelled face set (indicative) and open the BlazeFace PDF to confirm the weights licence (ADR 0021) before adopting it.
- [ ] H15 legal owner signs consent text and retention; until then uploads only to the team dev server from internal testers.

### Upload (field flavour)
- [ ] H16 `cd capturekit/dev-server && docker compose up -d`, phone on the same LAN, scan, Upload; pass: bundle reaches COMPLETE (`./smoke.sh` equivalent on the server).
- [ ] H17 airplane mode mid-upload then off; pass: resumes, state goes retrying then verified; My scans list shows the state; FAIL verdict hides Upload.

### Phase 9 (after U8-A/U11-A)
- [ ] H18 `adb shell setprop debug.capturekit.understanding 1` (and `debug.capturekit.detector 1`) then walk a furnished room with a door and a window; pass: map shows objects/openings, manifest counts detections > 0, `ckit validate --level quality` no new ERROR; flag off behaves as before.
- [ ] H19 detector latency per device (one device = one tier, say so) and the ADR 0024 on_plane_tol 0.04 check: no false openings punched into walls in 3 real rooms.
- [ ] H20 walk 3 real rooms before enabling gap steering (planner flag stays OFF until then).
- [ ] H21 read COCO terms and confirm weights redistribution (ADR 0017), else drop detector.

### Guidance tuning
- [ ] H22 `ckit replay <real bundle> --sink guide` and `ckit guide-eval <bundle>`; pass: instruction rate and completeness sane vs sim (ADR 0023); retune knobs through extraConfig `guide.*`.

### Trials (Phase 12)
- [ ] H23 follow capturekit/docs/trial-protocol.md: 3+ rooms, guided vs unguided, then `ckit e2e BUNDLE --research JSON` and `ckit trial-report DIR`; pass: report produced, Phase 12 targets compared once the owner sets them.

### Part 2
- [ ] H24 send 3-5 real bundles to roomscan-recon, tell them schema 1.1.0/1.2.0/1.3.0 are additive; pass: their reader accepts them.

### Owner decisions
- [ ] H25 confirm or change ADR 0018 items 1-17 (depth, tiers, locales, legal owner, Mac, detector scope, gaps flag, schema order, flavour, redaction, voice, wall names, haptics, camera image size, speed targets, Phase 12 targets); set APK size limit in scripts/apk_size.sh; device matrix (docs/device-matrix.md); TUS_URL and SENTRY_DSN values as gradle properties (never committed). Owner decisions line: set the APK size limit in scripts/apk_size.sh knowing the detector adds ~6 MB (LiteRT runtime) to BOTH flavours (code is in the shared lib; the model file itself is fetched, not committed); the measured debug APKs were 124 MB standard and 150 MB field (debug, all ABIs, unstripped), so the limit must be set on a release build.

### CI/iOS
- [ ] H26 push is yours: check the Actions page for core, sanitizers, python, upload, android (both flavours), schema-compat, ios jobs;
- [ ] H27 iOS needs a Mac or a green macOS job: first session = compile-and-fix only, then Swift ports of overlay, guidance HUD, voice, review, consent.

## Added at the end of the unattended run (code for these is in the uncommitted tree on top of b3d39f0)

### Unblocked by code (you can run them once the tree is built and installed)
H8 and H12 (voice: VoiceController, repeatLast, mic hidden below API 31), H13 and H15 (consent screen, Start gate, revoke), H14 (redaction, with `consent`/`privacy` in manifest 1.2.0), H16 and H17 (My scans, upload states), H23 (research log in the field build). Still blocked on code: H18 and H19 (debug flags `debug.capturekit.understanding` / `.detector` and the room shell/gap wiring are U11-A, not done; the detector worker exists but nothing starts it), H20 (U10-C not done), H22 (ADR 0023 not written).

### New device checks
- [ ] H28 Consent flow: fresh install shows the consent screen (placeholder text, "pending legal owner"); Start is blocked until accepted; "Withdraw consent" in Home returns to the blocked state. Pass: new bundle manifest has `consent.version`, `consent.at` and `privacy.redaction_mode = faces`; `ckit validate <bundle>` has no CONSENT_MISSING.
- [ ] H29 My scans and upload states: finish 3 scans (one weak enough for a FAIL verdict). Pass: list shows waiting/uploading/retrying/verified/failed; FAIL hides Upload; the unmetered-only toggle holds uploads on mobile data. Revoke consent while an upload is queued: pass = the queued upload does not start (check `adb logcat` and the dev server).
- [ ] H30 Sentry (field flavour only): build with `-PSENTRY_DSN=<dsn>` and accept telemetry consent; trigger a handled test error; pass: event arrives with no file paths or bundle names. Empty DSN or no telemetry consent: nothing sent. Standard flavour: no Sentry classes (`apk_size.sh`/merged manifest has no INTERNET).
- [ ] H31 Voice on API 31-32: these need the on-device English speech pack downloaded in system settings; note the SDK level and whether the pack was present; without the pack the mic must hide with a plain message. (Also see H12.)
- [ ] H32 Research log (field flavour): participant code, optional room label and arm at setup; after a scan `<bundle>.research.json` sits next to the bundle directory, is not inside it, and is not uploaded (check the server copy). Standard flavour shows no trial setup.
- [ ] H33 Detector flag and model: `capturekit/scripts/fetch_models.sh` (sha256 pinned) then rebuild; app runs identically with the model absent. The detector is OFF by default (ADR 0032): on a debug build run `adb shell setprop debug.capturekit.detector 1` and force-stop the app, then test model loading and latency per tier (H19).
- [ ] H34 AIM_TEXTURED prompt: scan a blank or low-texture wall (G8-like, tracking reason INSUFFICIENT_FEATURES). Pass: "Aim at a textured wall" is spoken and shown once, not repeated within the speech budget; EXCESSIVE_MOTION says slow down, INSUFFICIENT_LIGHT says more light.
- [ ] H35 Verdict text: `ckit verdict-export` output equals `sample-app/src/main/assets/verdict_messages.json`; read three verdict messages on the Review screen for wording.
- [ ] H36 Recalibration card (issue #1, ADR 0031; OFF by default, ADR 0032): debug build, `adb shell setprop debug.capturekit.recalibrate_card 1`, force-stop, start a scan, cover the lens for 3+ s, then uncover and turn slowly. Pass: the "Recalibrating" card appears within ~2 s of covering, no keyframe is saved meanwhile, the card dismisses within ~4 s of tracking returning, no crash. Then log it in DEVICE_TEST_LOG.md and flip the default in `FeatureFlags.Flags`.
- [ ] H37 Segmentation model on the phone (issue #8b; OFF by default): `capturekit/scripts/fetch_models.sh --with-segmentation` (sha256 pinned, ~97 MB), rebuild, debug build, `adb shell setprop debug.capturekit.segmentation 1`, force-stop, start a scan. Check in `adb logcat -s CK/Segment`: "segmenter ready: sam2.1-tiny enc=GPU|CPU dec=..." (GPU expected; CPU means ~5-10 s per run), per-run latency, drop count. Verify the encoder output order (image_embeddings vs feat_s1, same size; the code assumes card order): a mask must cover the prompted floor / object, not noise. Without the files the app runs exactly as before (no chip). Spike: box prompt as two corner points (8a uses the box centre point).
- [ ] H38 Segmentation overlay behaviour (issue #8b): with H37 passing, detector OFF: masks show floor / walls / ceiling only; detector ON (`debug.capturekit.detector 1`): objects get masks with labels, but only once ARCore has found an upward floor plane (object prompts take their height from the floor; no fallback, so no object masks before the floor is found). Turn the model files' failure into a check too: if the worker disables itself (logcat "disabled"), the Masks chip must go and the Boxes chip and boxes come back. With `guidance` on, tapping the room card now hides only its caption (the mini-map is gone). Pass: one "Masks" chip in the boxes chip's place toggles the overlay; the mini-map is gone from the capture screen (still on Review with `guidance`); an object or wall that leaves the view and comes back keeps its colour and label; masks follow a slow pan between runs (rotation warp) and disappear within ~3 s on pause / lost tracking; no frame-rate drop on the camera view; the bundle has no mask files or channels (`ckit inspect`). Then log it in DEVICE_TEST_LOG.md and decide the default.
- Feature switches (ADR 0032): guidance, detector, voice commands and the recalibration card are OFF in the default build. Before H9-H12, H18-H22, H31, H33 set the matching property (`debug.capturekit.guidance`, `.detector`, `.voice_commands`) to 1 on a debug build and force-stop the app; docs/STATUS.md section 0 maps each feature to its tests.
