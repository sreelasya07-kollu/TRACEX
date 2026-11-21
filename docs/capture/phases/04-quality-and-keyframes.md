# Phase 4: Quality metrics and keyframe selector
**Goal/why:** reconstruction is limited by the worst frames kept. Good keyframes also cut upload size, and the metrics later feed guidance.

## Decisions
- Gyro (early warning) + Laplacian (confirmation), normalized per scene. Rejected: Laplacian alone (blank walls look blurry), gyro alone (misses defocus). Learned IQA deferred.
- Motion thresholds + quality gate + best-in-window. Rejected: fixed interval. Coverage-gain term deferred to Phase 8.
- Versioned config recorded in the bundle. Rejected: constants in code.

## Tests first
Programmatically generated images (sharp, Gaussian blur, motion blur, over/under-exposed, blank wall, noisy) with asserted metric ordering and hard rejects.

## Build (C++20, luma plane + metadata, no platform deps)
1. Downscale luma to ~320 px wide. Metrics: variance-of-Laplacian sharpness normalized by a running session baseline; gyro motion-blur risk over the exposure window; exposure clipping %; FAST corner count and spatial spread; tracking state and reason.
2. Quality score with documented hard rejects and soft penalties, returning reason codes.
3. KeyframeSelector state machine: quality gate → novelty trigger (translation as a ratio of estimated scene depth with a documented indoor default, or rotation delta) → minimum gap → short candidate window emitting the best candidate.
4. Thresholds in a versioned config; write its hash to the manifest; write per-frame metrics and decision reasons to /frame_index and /events.
5. ReplaySink + `ckit quality-report` (keyframe density, rejection histogram, pycolmap registration rate of selected keyframes).

## Constraints
No per-frame heap allocation on the hot path. Target ≤3 ms per frame (document the desktop proxy benchmark).

## Acceptance
Tests pass. Quality reports for all golden sessions committed. Baselines added to CI.
