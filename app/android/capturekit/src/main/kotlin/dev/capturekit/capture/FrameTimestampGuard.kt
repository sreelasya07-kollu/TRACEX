package dev.capturekit.capture

/**
 * ARCore can hand out frames with timestamp 0 before the first real camera frame (scan started straight
 * after the consent screen). Such frames must never reach the core: they produced a pose/intrinsics at t_ns 0.
 * Pure (no ARCore types) so it is JVM-testable.
 */
class FrameTimestampGuard {
    var skipped = 0L
        private set

    /** True when the frame may be ingested. Returns false (and counts) for timestamp <= 0. */
    fun accept(frameTimestampNs: Long): Boolean {
        if (frameTimestampNs > 0L) return true
        skipped++
        return false
    }

    /** True exactly once, on the first skipped frame, so the caller logs one line. */
    fun firstSkip(): Boolean = skipped == 1L
}
