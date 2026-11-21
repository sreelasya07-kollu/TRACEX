package dev.capturekit.guidance

import dev.capturekit.guidance.StatusPill.statusPill
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class StatusPillTest {
    private val expected = mapOf(
        "TRACKING_LOST" to "Tracking lost",
        "TRACKING_LIMITED" to "Keep going",
        "MOTION_BLUR" to "Move slower",
        "BLUR" to "Hold steady",
        "SOFT_FOCUS" to "Hold steady",
        "OVEREXPOSED" to "Keep going",
        "UNDEREXPOSED" to "More light",
        "LOW_TEXTURE" to "Keep going",
        "POOR_SPREAD" to "Keep going",
        "NO_IMAGE" to "Keep going",
        "NOT_NOVEL" to "Good",
        "MIN_GAP" to "Good",
        "NOT_BEST" to "Good",
        "RECALIBRATING" to "Recalibrating",
        "DUPLICATE" to "Good",
    )

    @Test fun recalibratingOutranksEverythingButTrackingLost() {
        assertEquals(StatusPill.RECALIBRATING, statusPill("PAUSED", "INSUFFICIENT_FEATURES", listOf("MOTION_BLUR", "RECALIBRATING"), 4, everTracked = true))
        assertEquals(StatusPill.LOST, statusPill("STOPPED", "NONE", listOf("RECALIBRATING"), 0, everTracked = true))
    }

    @Test fun everyCoreReasonHasAPlainPhrase() {
        assertEquals(expected.keys, StatusPill.REASON_CODES.toSet())
        for (code in StatusPill.REASON_CODES) {
            val p = statusPill("TRACKING", "NONE", listOf(code), 0)
            assertEquals(code, expected[code], p.text)
            assertFalse(code, p.text.contains('_'))
        }
    }

    @Test fun unknownReasonIsKeepGoing() { assertEquals(StatusPill.KEEP_GOING, StatusPill.forReason("SOMETHING_NEW")) }

    @Test fun allClearIsGood() { assertEquals(StatusPill.GOOD, statusPill("TRACKING", "NONE", emptyList(), 0)) }

    @Test fun priorityOrder() {
        val all = listOf("UNDEREXPOSED", "SOFT_FOCUS", "MOTION_BLUR")
        assertEquals(StatusPill.LOST, statusPill("STOPPED", "NONE", all, 3))
        assertEquals(StatusPill.TOO_FAST, statusPill("TRACKING", "NONE", all, 3))
        assertEquals(StatusPill.HOLD, statusPill("TRACKING", "NONE", listOf("UNDEREXPOSED", "SOFT_FOCUS"), 3))
        assertEquals(StatusPill.LIGHT, statusPill("TRACKING", "NONE", listOf("UNDEREXPOSED", "NOT_NOVEL"), 3))
        assertEquals(StatusPill.WARMING, statusPill("TRACKING", "NONE", listOf("NOT_NOVEL"), 1))
        assertEquals(StatusPill.HOT, statusPill("TRACKING", "NONE", emptyList(), 4))
        assertEquals(StatusPill.KEEP_GOING, statusPill("TRACKING", "NONE", listOf("MIN_GAP", "LOW_TEXTURE"), 0))
    }

    @Test fun trackingFailuresMapToPlainPhrases() {
        assertEquals(StatusPill.TOO_FAST, statusPill("PAUSED", "EXCESSIVE_MOTION", emptyList(), 0))
        assertEquals(StatusPill.LIGHT, statusPill("PAUSED", "INSUFFICIENT_LIGHT", emptyList(), 0))
        assertEquals(StatusPill.HOLD, statusPill("PAUSED", "INSUFFICIENT_FEATURES", emptyList(), 0))
        assertEquals(StatusPill.STARTING, statusPill("?", "NONE", listOf("MOTION_BLUR"), 5))
        assertEquals(StatusPill.STARTING, statusPill("PAUSED", "NONE", emptyList(), 0))
    }

    @Test fun selectorReasonsNeverAffectThePill() {
        for (code in listOf("NOT_NOVEL", "MIN_GAP", "NOT_BEST", "DUPLICATE")) assertNull(code, StatusPill.forReason(code))
        assertEquals(StatusPill.GOOD, statusPill("TRACKING", "NONE", listOf("NOT_NOVEL", "MIN_GAP", "NOT_BEST"), 0))
        assertEquals(StatusPill.WARMING, statusPill("TRACKING", "NONE", listOf("NOT_BEST"), 2))
    }

    @Test fun startingOnlyBeforeFirstTracking() {
        assertEquals(StatusPill.STARTING, statusPill("PAUSED", "NONE", emptyList(), 0, everTracked = false))
        assertEquals(StatusPill.HOLD, statusPill("PAUSED", "NONE", emptyList(), 0, everTracked = true))
        assertEquals(StatusPill.HOLD, statusPill("?", "NONE", listOf("NOT_NOVEL"), 0, everTracked = true))
        assertEquals(StatusPill.TOO_FAST, statusPill("PAUSED", "NONE", listOf("MOTION_BLUR"), 0, everTracked = true))
    }

    @Test fun tonesMatchSeverity() {
        assertEquals(StatusPill.Tone.BAD, StatusPill.LOST.tone)
        assertEquals(StatusPill.Tone.GOOD, StatusPill.GOOD.tone)
        assertTrue(listOf(StatusPill.TOO_FAST, StatusPill.HOLD, StatusPill.LIGHT, StatusPill.WARMING).all { it.tone == StatusPill.Tone.WARN })
    }
}
