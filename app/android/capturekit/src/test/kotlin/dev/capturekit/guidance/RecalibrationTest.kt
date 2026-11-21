package dev.capturekit.guidance

import dev.capturekit.capture.CoreEvent
import dev.capturekit.capture.EventParser
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class RecalibrationTest {
    private fun recal(text: String) = CoreEvent.Instruction(7, "RECALIBRATE", "Turn your phone slowly in a small circle.", text, 3, "double", "recalibrate")
    private val lost = CoreEvent.TrackingLost(2_000_000_000L, 2, 3, 2.0)
    private val reset = CoreEvent.TrackingReset(5_000_000_000L, 3.5, 0.25, 4.0)

    @Test fun lostShowsResetHidesAndKeepsTheDriftDelta() {
        var r = Recalibration()
        assertFalse(r.active)
        r = r.on(lost)
        assertTrue(r.active)
        assertEquals(Recalibration.FALLBACK, r.prompt)   // no RECALIBRATE text yet
        r = r.on(recal("Tracking lost. Slowly turn your phone in a small circle to recalibrate."))
        assertEquals("Tracking lost. Slowly turn your phone in a small circle to recalibrate.", r.prompt)
        r = r.on(reset)
        assertFalse(r.active)
        assertEquals(1, r.resets)
        assertEquals(0.25, r.lastReset!!.driftM, 1e-12)
    }

    @Test fun instructionAloneNeverOpensThePromptAndOtherEventsAreIgnored() {
        val r = Recalibration().on(recal("x"))
        assertFalse(r.active)   // visibility follows the core's tracking events only
        val other = CoreEvent.Instruction(1, "MOVE_TO", "Walk ahead.", "", 2, "tap", "")
        val s = Recalibration(active = true, text = "t")
        assertSame(s, s.on(other))
        assertSame(s, s.on(lost))   // already active: unchanged
        assertSame(s, s.on(CoreEvent.KeyframeRequest(1, "keyframes/000001.jpg")))
    }

    @Test fun sessionEndClearsAndRepeatedLossesCount() {
        var r = Recalibration().on(lost).on(reset).on(lost)
        assertTrue(r.active)
        r = r.on(CoreEvent.StateChange("SCANNING", "PAUSED", "user"))
        assertTrue(r.active)   // a user pause does not end the recovery
        r = r.on(reset)
        assertEquals(2, r.resets)
        assertEquals(Recalibration(), r.on(lost).on(CoreEvent.StateChange("SCANNING", "FINALIZING", "stop")))
    }

    @Test fun parsedCoreLinesDriveIt() {
        val lines = """{"degraded_s":2,"reason":3,"t_ns":1234567890123,"tracking_state":2,"type":"tracking_lost"}
{"type":"tracking_reset","t_ns":1234567899999,"lost_s":3.5,"drift_m":0.12,"drift_deg":7.5}
"""
        val ev = EventParser.parse(lines)
        var r = Recalibration().on(ev[0])
        assertTrue(r.active)
        r = r.on(ev[1])
        assertFalse(r.active)
        assertEquals(7.5, r.lastReset!!.driftDeg, 1e-12)
    }
}
