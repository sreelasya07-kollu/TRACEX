package dev.capturekit.capture

/**
 * When the detector may run (pure, JVM-tested). One run at most every [baseIntervalNs] / ml_rate_scale of core time
 * (0.5 s at scale 1, longer under thermal pressure); never while the core asks for a pause, tracking is not NORMAL, or
 * the scale is 0. Time is the caller's monotonic ns clock (frame t_ns), never read here.
 */
class DetectorScheduler(private val baseIntervalNs: Long = 500_000_000L) {
    private var lastRunNs = Long.MIN_VALUE

    /** True when a run should start at [tNs]; the call then counts as that run. */
    fun shouldRun(tNs: Long, mlRateScale: Double, mustPause: Boolean, trackingNormal: Boolean): Boolean {
        if (mustPause || !trackingNormal || !(mlRateScale > 0.0)) return false
        if (lastRunNs != Long.MIN_VALUE) {
            if (tNs < lastRunNs) { lastRunNs = tNs; return false }   // clock went backwards: restart the interval
            if (tNs - lastRunNs < intervalNs(mlRateScale)) return false
        }
        lastRunNs = tNs
        return true
    }

    fun intervalNs(mlRateScale: Double): Long = (baseIntervalNs / mlRateScale).toLong()

    fun reset() { lastRunNs = Long.MIN_VALUE }
}

/** Bounded FIFO that drops the OLDEST entry when full and counts the drops. Thread-safe. */
class DropOldestQueue<T>(private val capacity: Int = 2) {
    private val items = ArrayDeque<T>()
    @Volatile var dropped = 0L
        private set

    init { require(capacity >= 1) }

    /** Adds [item]; returns the evicted oldest entry, or null when there was room. */
    @Synchronized
    fun offer(item: T): T? {
        var evicted: T? = null
        if (items.size >= capacity) { evicted = items.removeFirst(); dropped++ }
        items.addLast(item)
        return evicted
    }

    @Synchronized fun poll(): T? = if (items.isEmpty()) null else items.removeFirst()
    @Synchronized fun size() = items.size
    @Synchronized fun clear() = items.clear()
}

/** Box mapping between the upright image the model sees and the stored keyframe image. */
object BoxMapping {
    /** Rect in stored-image pixels (float, x0 < x1, y0 < y1). */
    data class PixelBox(val x0: Float, val y0: Float, val x1: Float, val y1: Float)

    /**
     * Maps a normalised box of the UPRIGHT image (ymin, xmin, ymax, xmax each 0..1, the model's order) back to the
     * stored image of [storedW] x [storedH] pixels. [rotationDeg] is the clockwise rotation (0, 90, 180, 270) that turns
     * the stored image upright; the upright image is storedH x storedW wide x tall for 90 and 270. Coordinates are
     * clamped to the image; returns null for a box with no area after clamping or an unsupported rotation.
     */
    fun toStored(ymin: Float, xmin: Float, ymax: Float, xmax: Float, rotationDeg: Int, storedW: Int, storedH: Int): PixelBox? {
        val w = storedW.toFloat(); val h = storedH.toFloat()
        val u0 = xmin.coerceIn(0f, 1f); val u1 = xmax.coerceIn(0f, 1f)
        val v0 = ymin.coerceIn(0f, 1f); val v1 = ymax.coerceIn(0f, 1f)
        val b = when (((rotationDeg % 360) + 360) % 360) {
            0 -> PixelBox(u0 * w, v0 * h, u1 * w, v1 * h)
            // upright (rx, ry) = (H - y, x) for 90 clockwise, so x = ry, y = H - rx; rx = u * H, ry = v * W
            90 -> PixelBox(v0 * w, h - u1 * h, v1 * w, h - u0 * h)
            180 -> PixelBox(w - u1 * w, h - v1 * h, w - u0 * w, h - v0 * h)
            // 270 clockwise (= 90 counter-clockwise): x = W - ry, y = rx
            270 -> PixelBox(w - v1 * w, u0 * h, w - v0 * w, u1 * h)
            else -> return null
        }
        return if (b.x1 - b.x0 > 0f && b.y1 - b.y0 > 0f) b else null
    }

    /**
     * Clockwise rotation that makes a keyframe upright, from the camera sensor orientation and the display rotation,
     * both in degrees (sensor 90 or 270 on phones; display 0/90/180/270). Back camera.
     */
    fun uprightRotation(sensorOrientationDeg: Int, displayRotationDeg: Int): Int =
        (((sensorOrientationDeg - displayRotationDeg) % 360) + 360) % 360
}
