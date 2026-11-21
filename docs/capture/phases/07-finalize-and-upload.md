# Phase 7: Finalization, validation and upload
**Goal/why:** a scan only has value if it arrives intact over bad networks.

## Decisions
- tus. Rejected: direct S3 multipart, custom chunked HTTP.
- Manifest uploaded last; server verifies hashes. Rejected: inferring completeness from file count, a lossy "done" call.
- Upload with a verdict attached: ERROR prompts a rescan, WARN proceeds with a note. Rejected: blocking upload on failure.

## Build
1. finalize(): close MCAP, sha256 every file, run the on-device validator subset (structural, integrity, semantic), write manifest.json last. Map each validator code to a verdict (PASS/WARN/FAIL) and a plain-language instruction (code-to-message table + docs/verdict-messages.md).
2. iOS TUSKit uploads (one per file, manifest last, resume after kill/reboot, background-capable). Android tus-android-client under WorkManager with network constraints and exponential backoff.
3. Dev-server: tusd post-finish hook + small verifier marking a bundle COMPLETE only when the manifest arrived and every listed file matches sha256 and size; otherwise INCOMPLETE or CORRUPT with a reason.
4. Local retention: delete local files only after server verification and only if the user setting allows. No upload without a recorded consent flag (stub; Part I owns the full flow).
5. Failure-matrix tests (docker compose where possible, else docs/upload-test-protocol.md): airplane mode, app kill, reboot, server restart, truncated file, corrupted chunk, tampered file, disk full.

## Acceptance
Every failure case ends in a COMPLETE verified bundle or a clearly reported failure. dev-server/smoke.sh runs end to end in CI.
