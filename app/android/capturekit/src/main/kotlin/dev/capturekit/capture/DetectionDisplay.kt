package dev.capturekit.capture

import kotlin.math.max
import kotlin.math.min

/**
 * Pure helpers for drawing detector boxes over the live camera feed (JVM-tested; the view lives in the sample app).
 * Boxes are in stored-keyframe pixels = ARCore CPU-image pixels (Coordinates2d.IMAGE_PIXELS). The host maps the two
 * opposite corners of each box with Frame.transformCoordinates2d(IMAGE_PIXELS -> VIEW) and turns them back into
 * axis-aligned view rectangles with [rects]: the display transform is a multiple of 90 degrees plus scale and offset,
 * so a rotation can swap which corner is top-left but the min / max of the two is the exact rectangle.
 * A keyframe's boxes are up to a detector interval old and the camera keeps moving: they are shown briefly, then fade.
 */
object DetectionDisplay {
    /** Total time a set of boxes stays on screen after it arrives. */
    const val SHOW_MS = 1500L
    /** The last part of [SHOW_MS] over which the boxes fade out. */
    const val FADE_MS = 500L
    /** At most this many boxes are drawn per keyframe (the worker caps at 20; the model order is best first). */
    const val MAX_BOXES = 12

    /** Opacity 0..1 of boxes [ageMs] old: 1 until SHOW_MS - FADE_MS, then linear to 0 at SHOW_MS; 0 when negative or older. */
    fun alpha(ageMs: Long): Float = when {
        ageMs < 0 || ageMs >= SHOW_MS -> 0f
        ageMs <= SHOW_MS - FADE_MS -> 1f
        else -> (SHOW_MS - ageMs).toFloat() / FADE_MS
    }

    /** Writes (x0, y0, x1, y1) of the first [MAX_BOXES] boxes into [out] (4 floats each); returns the box count. */
    fun corners(boxes: List<DetectedBox>, out: FloatArray): Int {
        val n = min(min(boxes.size, MAX_BOXES), out.size / 4)
        for (i in 0 until n) {
            val b = boxes[i]
            out[4 * i] = b.x0; out[4 * i + 1] = b.y0; out[4 * i + 2] = b.x1; out[4 * i + 3] = b.y1
        }
        return n
    }

    /**
     * Mapped corner pairs (as returned by transformCoordinates2d for [corners]) to (left, top, right, bottom) in [out]
     * for [n] boxes. Returns how many rectangles are valid; a box with a non-finite or zero-area result is skipped and the
     * following ones move up, so the first returned entries are the drawable ones in input order. [keep] receives the
     * input index of each kept rectangle (for its label).
     */
    fun rects(mapped: FloatArray, n: Int, out: FloatArray, keep: IntArray): Int {
        var k = 0
        for (i in 0 until min(n, min(mapped.size / 4, min(out.size / 4, keep.size)))) {
            val ax = mapped[4 * i]; val ay = mapped[4 * i + 1]; val bx = mapped[4 * i + 2]; val by = mapped[4 * i + 3]
            if (!(ax.isFinite() && ay.isFinite() && bx.isFinite() && by.isFinite())) continue
            val l = min(ax, bx); val r = max(ax, bx); val t = min(ay, by); val b = max(ay, by)
            if (r - l <= 0f || b - t <= 0f) continue
            out[4 * k] = l; out[4 * k + 1] = t; out[4 * k + 2] = r; out[4 * k + 3] = b
            keep[k] = i
            k++
        }
        return k
    }
}
