package dev.capturekit.capture

/**
 * Candidate-rate control for the AR thread (pure JVM, no locks, never blocks). Forwards every [stride]-th
 * camera frame to the core. Pressure (a FULL ingest status, or core policy level >= [pressureLevel]) doubles
 * the stride up to [maxStride]; after [cleanNs] of frame time without pressure it returns to [baseStride].
 * Time is the frame timestamp passed in, never a clock read.
 */
class FrameRateControl(
    val baseStride: Int = 3,
    val maxStride: Int = 6,
    private val cleanNs: Long = 10_000_000_000L,
    private val pressureLevel: Int = 2,
) {
    init { require(baseStride >= 1 && maxStride >= baseStride) }

    var stride = baseStride
        private set
    private var counter = 0
    private var lastPressureNs = Long.MIN_VALUE

    /** True when this frame should become a core candidate. */
    fun take(): Boolean {
        if (++counter < stride) return false
        counter = 0
        return true
    }

    /**
     * Report the state at frame time [tNs]: [full] = the last ingest returned FULL, [policyLevel] = latest core
     * policy level. Returns the new stride if it changed, else null (the caller logs changes).
     */
    fun update(tNs: Long, full: Boolean, policyLevel: Int): Int? {
        val old = stride
        if (full || policyLevel >= pressureLevel) {
            lastPressureNs = tNs
            stride = minOf(maxStride, stride * 2)
        } else if (stride != baseStride && lastPressureNs != Long.MIN_VALUE && tNs - lastPressureNs >= cleanNs) {
            stride = baseStride
        }
        return if (stride != old) stride else null
    }
}
