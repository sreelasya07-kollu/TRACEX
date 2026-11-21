package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class BoxMappingTest {
    // Stored image 640 wide x 480 tall. A marker box near the stored top-left corner: x 40..120, y 20..60.
    private val w = 640
    private val h = 480

    private fun assertBox(b: BoxMapping.PixelBox?, x0: Float, y0: Float, x1: Float, y1: Float) {
        assertNotNull(b)
        assertEquals(x0, b!!.x0, 0.01f)
        assertEquals(y0, b.y0, 0.01f)
        assertEquals(x1, b.x1, 0.01f)
        assertEquals(y1, b.y1, 0.01f)
    }

    @Test fun rotation0IsPlainScaling() {
        assertBox(BoxMapping.toStored(20f / h, 40f / w, 60f / h, 120f / w, 0, w, h), 40f, 20f, 120f, 60f)
    }

    @Test fun rotation90ClockwiseUpright() {
        // Upright image (rotated 90 cw) is 480 wide x 640 tall. Stored point (x, y) -> upright (h - y, x).
        // Marker x 40..120, y 20..60 -> upright x 420..460, y 40..120.
        assertBox(BoxMapping.toStored(40f / w, 420f / h, 120f / w, 460f / h, 90, w, h), 40f, 20f, 120f, 60f)
    }

    @Test fun rotation270Upright() {
        // 270 cw: stored (x, y) -> upright (y, w - x). Marker -> upright x 20..60, y 520..600.
        assertBox(BoxMapping.toStored(520f / w, 20f / h, 600f / w, 60f / h, 270, w, h), 40f, 20f, 120f, 60f)
    }

    @Test fun rotation180Upright() {
        // 180: stored (x, y) -> (w - x, h - y). Marker -> upright x 520..600, y 420..460.
        assertBox(BoxMapping.toStored(420f / h, 520f / w, 460f / h, 600f / w, 180, w, h), 40f, 20f, 120f, 60f)
    }

    @Test fun clampsToImageAndRejectsEmptyOrUnsupported() {
        assertBox(BoxMapping.toStored(-0.2f, -0.1f, 0.5f, 1.3f, 0, w, h), 0f, 0f, 640f, 240f)
        assertNull(BoxMapping.toStored(0.5f, 0.5f, 0.5f, 0.9f, 0, w, h))
        assertNull(BoxMapping.toStored(0.9f, 0.5f, 0.1f, 0.9f, 0, w, h))   // inverted
        assertNull(BoxMapping.toStored(0.1f, 0.1f, 0.5f, 0.5f, 45, w, h))
        assertNull(BoxMapping.toStored(1.4f, 0.1f, 1.8f, 0.5f, 0, w, h))   // fully outside
    }

    @Test fun negativeRotationNormalises() {
        assertBox(BoxMapping.toStored(40f / w, 420f / h, 120f / w, 460f / h, -270, w, h), 40f, 20f, 120f, 60f)
    }
}
