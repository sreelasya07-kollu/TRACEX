package dev.capturekit.capture

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class DetectionDisplayTest {
    @Test fun alphaHoldsThenFadesThenHides() {
        assertEquals(0f, DetectionDisplay.alpha(-1), 0f)
        assertEquals(1f, DetectionDisplay.alpha(0), 0f)
        assertEquals(1f, DetectionDisplay.alpha(DetectionDisplay.SHOW_MS - DetectionDisplay.FADE_MS), 0f)
        assertEquals(0.5f, DetectionDisplay.alpha(DetectionDisplay.SHOW_MS - DetectionDisplay.FADE_MS / 2), 1e-6f)
        assertEquals(0f, DetectionDisplay.alpha(DetectionDisplay.SHOW_MS), 0f)
        assertEquals(0f, DetectionDisplay.alpha(Long.MAX_VALUE), 0f)
    }

    @Test fun cornersAreCappedAndInInputOrder() {
        val boxes = (0 until 20).map { DetectedBox(1, 0.9f, it.toFloat(), 1f, it + 10f, 11f) }
        val out = FloatArray(4 * 20)
        val n = DetectionDisplay.corners(boxes, out)
        assertEquals(DetectionDisplay.MAX_BOXES, n)
        assertArrayEquals(floatArrayOf(3f, 1f, 13f, 11f), out.copyOfRange(12, 16), 0f)
        assertEquals(1, DetectionDisplay.corners(boxes, FloatArray(7)))   // never writes past the buffer
    }

    /** A 90 degree clockwise display (portrait phone, sensor-oriented CPU image): corners swap, the rect must not invert. */
    @Test fun rectsAreAxisAlignedAfterARotationAndSkipBadBoxes() {
        val h = 480f
        fun map(x: Float, y: Float) = floatArrayOf(2f * (h - y), 2f * x)   // view = (h - y, x) rotated 90 cw, scale 2
        val stored = floatArrayOf(
            100f, 50f, 300f, 250f,   // ok
            10f, 10f, 10f, 40f,      // zero width -> skipped
            200f, 100f, 400f, 300f,  // ok
        )
        val mapped = FloatArray(stored.size)
        for (i in 0 until 6) { val m = map(stored[2 * i], stored[2 * i + 1]); mapped[2 * i] = m[0]; mapped[2 * i + 1] = m[1] }
        mapped[4] = Float.NaN   // the skipped box is also non-finite: still skipped, never drawn
        val out = FloatArray(12); val keep = IntArray(3)
        val k = DetectionDisplay.rects(mapped, 3, out, keep)
        assertEquals(2, k)
        assertArrayEquals(floatArrayOf(2f * (h - 250f), 200f, 2f * (h - 50f), 600f), out.copyOfRange(0, 4), 1e-3f)
        assertArrayEquals(floatArrayOf(2f * (h - 300f), 400f, 2f * (h - 100f), 800f), out.copyOfRange(4, 8), 1e-3f)
        assertEquals(0, keep[0]); assertEquals(2, keep[1])
    }
}
