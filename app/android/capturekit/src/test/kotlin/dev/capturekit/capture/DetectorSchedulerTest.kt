package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectorSchedulerTest {
    private val ms = 1_000_000L

    @Test fun firstRunImmediatelyThenEveryHalfSecond() {
        val s = DetectorScheduler()
        assertTrue(s.shouldRun(10_000 * ms, 1.0, false, true))
        assertFalse(s.shouldRun(10_400 * ms, 1.0, false, true))
        assertTrue(s.shouldRun(10_500 * ms, 1.0, false, true))
    }

    @Test fun intervalGrowsWithThermalScale() {
        val s = DetectorScheduler()
        assertEquals(1_000 * ms, s.intervalNs(0.5))
        assertTrue(s.shouldRun(0, 0.5, false, true))
        assertFalse(s.shouldRun(900 * ms, 0.5, false, true))
        assertTrue(s.shouldRun(1_000 * ms, 0.5, false, true))
    }

    @Test fun pausedLostTrackingOrZeroScaleNeverRunAndDoNotConsumeTheSlot() {
        val s = DetectorScheduler()
        assertFalse(s.shouldRun(0, 1.0, true, true))
        assertFalse(s.shouldRun(0, 1.0, false, false))
        assertFalse(s.shouldRun(0, 0.0, false, true))
        assertFalse(s.shouldRun(0, Double.NaN, false, true))
        assertTrue(s.shouldRun(1, 1.0, false, true))
    }

    @Test fun clockGoingBackwardsRestartsTheInterval() {
        val s = DetectorScheduler()
        assertTrue(s.shouldRun(5_000 * ms, 1.0, false, true))
        assertFalse(s.shouldRun(1_000 * ms, 1.0, false, true))
        assertTrue(s.shouldRun(1_500 * ms, 1.0, false, true))
    }

    @Test fun queueDropsOldestAndCounts() {
        val q = DropOldestQueue<Int>(2)
        assertNull(q.offer(1))
        assertNull(q.offer(2))
        assertEquals(1, q.offer(3))
        assertEquals(1L, q.dropped)
        assertEquals(2, q.poll())
        assertEquals(3, q.poll())
        assertNull(q.poll())
    }

    @Test fun uprightRotationFromSensorAndDisplay() {
        assertEquals(90, BoxMapping.uprightRotation(90, 0))
        assertEquals(0, BoxMapping.uprightRotation(90, 90))
        assertEquals(270, BoxMapping.uprightRotation(270, 0))
        assertEquals(180, BoxMapping.uprightRotation(270, 90))
        assertEquals(270, BoxMapping.uprightRotation(0, 90))
    }
}
