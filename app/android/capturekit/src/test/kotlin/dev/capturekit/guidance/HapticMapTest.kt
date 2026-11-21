package dev.capturekit.guidance

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class HapticMapTest {
    @Test fun noneIsSilent() { assertTrue(HapticMap.pulses("none").isEmpty()) }

    @Test fun tapIsOneTick() {
        assertEquals(listOf(HapticMap.Pulse(HapticMap.Kind.TICK, 0)), HapticMap.pulses("tap"))
    }

    @Test fun doubleIsTwoTicksSpaced() {
        val p = HapticMap.pulses("double")
        assertEquals(2, p.size); assertEquals(0L, p[0].delayMs); assertTrue(p[1].delayMs in 80..300)
        assertTrue(p.all { it.kind == HapticMap.Kind.TICK })
    }

    @Test fun successIsConfirm() {
        assertEquals(listOf(HapticMap.Pulse(HapticMap.Kind.CONFIRM, 0)), HapticMap.pulses("success"))
    }

    @Test fun unknownNameFallsBackToSilentUnlessUrgent() {
        assertTrue(HapticMap.pulses("weird", priority = 2).isEmpty())
        assertEquals(2, HapticMap.pulses("weird", priority = 3).size)
    }
}
