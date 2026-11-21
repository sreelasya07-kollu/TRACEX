package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class IngestStatsTest {
    @Test fun hudLineFormat() {
        val s = IngestStats()
        repeat(640) { s.arFrames.incrementAndGet() }
        repeat(612) { s.pose(true, "NONE") }
        repeat(28) { s.pose(false, "INSUFFICIENT_LIGHT") }
        repeat(190) { s.framesIngested.incrementAndGet() }
        repeat(14) { s.keyframes.incrementAndGet() }
        s.pointsIngested.addAndGet(33); repeat(4) { s.planesIngested.incrementAndGet() }
        assertEquals("trk 612/640 frm 190 kf 14 pts 33 pl 4", s.hudLine())
    }

    @Test fun logLineCarriesFailuresAndStatuses() {
        val s = IngestStats()
        s.acquireFailed(IllegalStateException("no image")); s.acquireFailed(IllegalStateException("again"))
        s.ingestStatus(0); s.ingestStatus(0); s.ingestStatus(3)
        s.pose(false, "EXCESSIVE_MOTION")
        val l = s.logLine()
        assertTrue(l, l.contains("acqFail IllegalStateException=2"))
        assertTrue(l, l.contains("ingestSt 0=2,3=1"))
        assertTrue(l, l.contains("reason EXCESSIVE_MOTION"))
    }

    @Test fun lastErrorClearsAfterCleanInterval() {
        val s = IngestStats()
        s.acquireFailed(IllegalStateException("boom"))
        s.endInterval()                       // interval that saw the error keeps it
        assertTrue(s.logLine().contains("lastErr"))
        s.endInterval()                       // next interval had no new error
        assertTrue(!s.logLine().contains("lastErr"))
    }

    @Test fun glCountersAndRateFormat() {
        val s = IngestStats()
        repeat(240) { s.glDraws.incrementAndGet() }
        repeat(120) { s.arDupFrames.incrementAndGet() }
        repeat(7) { s.glSkippedDup.incrementAndGet() }
        assertEquals("gl 240 (120.0/s) dup 120 skipped 7", s.glLine(2000, 240))
        assertEquals("0.0", IngestStats.perSecond(5, 0))
        assertEquals("29.9", IngestStats.perSecond(598, 20_000))
        assertTrue(s.logLine().contains("gl 240 dup 120"))
    }

    @Test fun lumaCapacityCoversStrideTimesHeight() {
        // Y plane buffers end at stride*(h-1)+w; the core requires stride*h.
        assertEquals(1280 * 720 + 0, IngestStats.lumaCapacity(1280 * 719 + 1270, 1280, 720))
        assertEquals(1000, IngestStats.lumaCapacity(1000, 10, 50))
    }
}
