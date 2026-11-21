package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Test

class LiveSessionsTest {
    @Test fun liveDirsAreNotRecoverableUntilRemoved() {
        val live = "/bundles/scan_live_test"; val dead = "/bundles/scan_dead_test"
        LiveSessions.add(live)
        try {
            assertEquals(listOf(dead), LiveSessions.recoverable(listOf(live, dead)))
        } finally { LiveSessions.remove(live) }
        assertEquals(listOf(live, dead), LiveSessions.recoverable(listOf(live, dead)))
    }
}
