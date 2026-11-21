package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class FrameRateControlTest {
    private val s = 1_000_000_000L

    @Test fun takesEveryStrideFrame() {
        val c = FrameRateControl(baseStride = 3, maxStride = 6)
        assertEquals(listOf(false, false, true, false, false, true), (1..6).map { c.take() })
    }

    @Test fun fullDoublesStrideBoundedAndRecoversAfterTenCleanSeconds() {
        val c = FrameRateControl(baseStride = 3, maxStride = 6)
        assertNull(c.update(0, full = false, policyLevel = 0))
        assertEquals(6, c.update(1 * s, full = true, policyLevel = 0))
        assertNull(c.update(2 * s, full = true, policyLevel = 0))       // bounded at max
        assertEquals(6, c.stride)
        assertNull(c.update(11 * s, full = false, policyLevel = 0))     // 9 s clean: not yet
        assertEquals(3, c.update(12 * s, full = false, policyLevel = 0)) // 10 s clean
        assertEquals(listOf(false, false, true), (1..3).map { c.take() })
    }

    @Test fun policyLevelTwoOrMoreIsPressure() {
        val c = FrameRateControl()
        assertNull(c.update(0, full = false, policyLevel = 1))
        assertEquals(6, c.update(s, full = false, policyLevel = 2))
        assertNull(c.update(20 * s, full = false, policyLevel = 3))      // still hot: stays slow
        assertEquals(3, c.update(31 * s, full = false, policyLevel = 0))
    }
}
