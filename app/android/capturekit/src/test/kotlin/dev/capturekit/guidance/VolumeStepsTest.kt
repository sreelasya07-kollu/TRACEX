package dev.capturekit.guidance

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class VolumeStepsTest {
    @Test fun defaultIsFullAndLouderIsRefused() {
        val v = VolumeSteps()
        assertEquals(1.0f, v.volume, 0f)
        assertFalse(v.louder())
        assertEquals(1.0f, v.volume, 0f)
    }

    @Test fun quieterWalksDownAndStopsAboveSilence() {
        val v = VolumeSteps()
        var n = 0
        while (v.quieter()) n++
        assertEquals(VolumeSteps.LEVELS.size - 1, n)
        assertTrue(v.volume > 0f)
        assertFalse(v.quieter())
    }

    @Test fun louderRestoresAndOutOfRangeStartIsClamped() {
        val v = VolumeSteps(-5)
        assertEquals(VolumeSteps.LEVELS.first(), v.volume, 0f)
        assertTrue(v.louder())
        assertTrue(v.volume > VolumeSteps.LEVELS.first())
        assertEquals(VolumeSteps.LEVELS.last(), VolumeSteps(99).volume, 0f)
    }
}
