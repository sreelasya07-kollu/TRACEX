package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameTimestampGuardTest {
    @Test fun zeroAndNegativeAreSkippedAndCounted() {
        val g = FrameTimestampGuard()
        assertFalse(g.accept(0L)); assertTrue(g.firstSkip())
        assertFalse(g.accept(-5L)); assertFalse(g.firstSkip())
        assertEquals(2L, g.skipped)
    }

    @Test fun realTimestampsPass() {
        val g = FrameTimestampGuard()
        assertTrue(g.accept(484_000_000_000_000L))
        assertEquals(0L, g.skipped)
    }
}
