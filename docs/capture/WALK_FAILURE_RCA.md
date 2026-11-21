# Why the first real walk failed (2026-11-18)

Evidence: bundle `capturekit/build/device/bundles/scan_20261117_215819` (4:17, 118 keyframes, validate PASS,
tracking 100 %), its guidance log, the Review screenshot (8 loose wall panels, "6 done, 1 scanning, 1 just found",
no ceiling). The phone, ARCore tracking and the recorded data are NOT the cause.

## Technical causes (most important first)
1. **No room model; "wall" = one ARCore plane fragment.** ARCore splits a wall wherever furniture hides it.
   `core/guide/live_map.cpp` add_plane merges two pieces only if normals are within 10°, offsets within 0.10 m and
   the gap is ≤ 0.30 m. Behind a sofa or cupboard the gap is larger, so one wall becomes 2–3 "walls" (8 for a
   4-wall room). Planes ARCore later subsumes are never removed, leaving stale duplicates.
2. **Hidden walls are never inferred on the phone.** RoomShell / Phase 9 understanding (wall line from the floor
   edge + fragments) is behind the `understanding` param, default 0 on device. A plain painted wall with little
   texture often gives ARCore no vertical plane at all.
3. **The completeness target cannot be reached.** Each 10 cm cell needs 3 keyframe views with 15° spread
   (`guide/params.hpp`), over the plane's whole bounding rectangle, including parts hidden behind furniture.
   `Coverage::sync` deletes a grid (and its statistics) when its plane id disappears, and grid axes are fixed at
   creation while ARCore keeps refining the plane. Result: completeness 3–7 % for 4 minutes, coverage fell
   0.42 → 0.25, and the planner kept sending the user back to areas that are physically blocked.
4. **Guidance was weak even in our own simulator.** ADR 0023 records that the planner never beats random
   wandering and furniture never reaches 90 %; it shipped unchanged. On device: 69 prompts in 257 s (budget
   8/min), 42 superseded (targets churn as plane ids change), no tilt-up prompt while the ceiling is required.
5. **The room picture can't look like a room.** RoomView draws each raw plane rectangle as a wall quad of fixed
   height: no corners, no closed outline, no furniture. The reference image (closed walls + furniture boxes) is a
   semantic room model; nothing on the phone builds one yet, and the object detector is off by default.
6. **Arrow (suspect, not yet replay-verified).** The mini-map is world-fixed, not heading-up, and world z is drawn
   screen-down, so the arrow does not turn the way the user faces; landscape makes it worse.

## Process / agent-setup causes (yes, they contributed)
1. **Acceptance was synthetic only.** The simulator makes clean, complete planes: no furniture occlusion, no
   fragmentation, no ARCore merges. 177 green tests said nothing about real rooms. Gate 0 (record 10+ real rooms
   before building guidance, H1) was skipped; the first real walk came after Phases 0–11 were marked done.
2. **"Done" meant builds + unit tests + a read-only diff review.** The UI/UX phase was marked done with no device
   screenshot and no person using it. Reviewers cannot run the app; many findings were "UNVERIFIED".
3. **Human gate bypassed.** While unattended, the planner settled product questions itself (ADR 0018):
   thresholds, understanding off, ceiling required. CLAUDE.md says to show Goal/Settled/Questions and wait.
4. **Red flags recorded, not acted on.** ADR 0023 measured the failure in the simulator and concluded "no
   change". AGENTS.md says to deviate when a measurement contradicts a decision; we didn't.
5. **Work sliced by file lane (core / android / tools), not by user outcome.** No task owned "a person scans a
   furnished room in ≤ 3 min", so nobody tested it end to end.
6. **Briefs froze the approach.** "Don't redo Settled" + "decisions are made" kept agents from challenging
   plane-id-based coverage.
7. **Model routing.** All agents ran Sonnet; AGENTS.md says geometry, coverage and planner logic need the
   strongest model. A contributing factor, not the main one.
8. **Orchestrator mistakes (Claude):** spawn-cwd guard failures, coordinate-based adb taps, and accepting
   "UI done" without asking for a device check.

The agent pipeline wrote what the briefs and simulator asked for. What failed was the definition of done
and the lack of real-world feedback; the agent setup made both easier to miss.

## Proposed way forward (nothing started; needs approval)
1. Record 5–10 furnished rooms (incl. this one) → replay fixtures; acceptance = replays of real rooms + a walk.
2. Room-layout model: cluster vertical fragments + floor edge + points into wall lines (merge across occlusion
   gaps, Manhattan prior), close the outline, mark spans hidden by furniture as "blocked", not "missing".
   Turn RoomShell on.
3. Completeness per wall from viewing angles (yaw sectors), not 3 views per 10 cm cell; keep coverage when
   planes merge or re-id.
4. Guidance: ≤ 6 prompts/min, hold a target, explicit "tilt up" step, ceiling optional for completion.
5. UI: heading-up mini-map, closed floor plan with blocked spans, objects when the detector is on.
6. Process: each UI/guidance brief needs device evidence (screenshot or video you approve); Opus for
   geometry/planner/coverage; the planner may not settle product decisions without you.
