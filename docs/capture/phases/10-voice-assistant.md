# Phase 10: Voice assistant
**Goal/why:** the user scans with eyes on the room, not the screen. Instructions must be spoken quickly, and the user must be able to talk back (pause, repeat, what's missing) without touching the phone.
Read docs/program-spec/part1-room-capture.md §2.5.

## Decisions
- Urgent lines: local templates + platform TTS (AVSpeechSynthesizer, Android TextToSpeech), target < ~0.5 s from Instruction to audio. Rejected: cloud TTS or LLM for urgent lines.
- Commands: on-device speech recognition with a closed intent grammar (pause, resume, repeat, skip, how much is left, what's missing, I'm done). Rejected: open-ended cloud STT for commands.
- Optional LLM only for non-urgent phrasing and questions, via a backend dialog proxy (no keys on the phone), grounded in a structured scan-state snapshot, must cite regions and abstain. Template fallback offline. Rejected: LLM judging coverage.
- Speech budget is a config value (prompts per minute, cooldowns) tuned with users. Rejected: hard-coded rates.

## Tests first
- Intent parser: table-driven utterance → intent tests (incl. noise, partial phrases), multilingual vocabulary files.
- Speech scheduler: priority preemption (blur interrupts a hint), barge-in stops speech, cooldowns, no repeats, budget respected — scripted timelines, deterministic.
- Grounded Q&A: answers built only from the snapshot; unanswerable questions abstain (fixture-based, model mocked).

## Build
1. C++ SpeechScheduler: Instruction queue → utterance plan (text, priority, interruptible) with budget and cooldowns; recorded in guidance log.
2. Platform audio: TTS adapters, audio session/ducking, haptic and tone renderers, microphone permission with explanation.
3. Voice I/O: VAD, on-device recognition (Speech framework / Android SpeechRecognizer on-device), barge-in, echo handling while speaking.
4. Intent handling wired to the session state machine (pause/resume/finish) and to state queries (coverage left, gaps by landmark).
5. Optional dialog client: scan-state snapshot (coverage, gaps, labels; no images) → proxy → phrased answer with region ids; strict timeouts; template fallback. Localization files for all templates.

## Constraints
No network needed for any urgent or command path. Audio consent recorded; no raw audio stored by default; transcripts redacted.

## Acceptance
Scheduler and intent tests pass. Latency measured on desktop proxy and documented for devices (device runs blocked on hardware). guide-eval reports prompts/min and repetition rate within the configured budget.
