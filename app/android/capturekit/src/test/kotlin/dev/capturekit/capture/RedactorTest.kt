package dev.capturekit.capture

import java.io.File
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RedactorTest {
    private class Fake(val result: List<Region>?, val boom: Boolean = false) : FaceRegionDetector {
        override val name = "fake"
        override fun detect(nv21: ByteArray, w: Int, h: Int): List<Region>? { if (boom) error("x"); return result }
    }

    private val w = 64; private val h = 48
    private fun frame() = ByteArray(w * h * 3 / 2) { (it % 200 + 30).toByte() }

    @Test fun changesPixelsInsideRegionAndLeavesOutsideIdentical() {
        val before = frame(); val buf = before.copyOf()
        val e = Redactor(Fake(listOf(Region(20, 10, 40, 30)), false), padFraction = 0.0).redact(buf, w, h, 1, "keyframes/a.jpg")
        assertEquals(listOf(Region(20, 10, 40, 30)), e.regions)
        for (y in 0 until h) for (x in 0 until w) {
            val inside = x in 20 until 40 && y in 10 until 30
            if (inside) assertEquals(Redactor.FILL_Y, buf[y * w + x]) else assertEquals(before[y * w + x], buf[y * w + x])
        }
        for (y in 0 until h / 2) for (x in 0 until w) {
            val i = w * h + y * w + x
            val inside = x in 20 until 40 && y in 5 until 15
            if (inside) assertEquals(Redactor.FILL_C, buf[i]) else assertEquals(before[i], buf[i])
        }
    }

    @Test fun paddingIsClampedAndEven() {
        val buf = frame()
        val e = Redactor(Fake(listOf(Region(1, 1, 63, 47))), padFraction = 0.5).redact(buf, w, h, 2, "k")
        val r = e.regions.single()
        assertEquals(Region(0, 0, 64, 48), r)
    }

    @Test fun detectorFailureFailsClosedAndIsRecorded() {
        val before = frame(); val buf = before.copyOf()
        val red = Redactor(Fake(null))
        assertTrue(red.redact(buf, w, h, 3, "k3").dropped)
        assertTrue(Redactor(Fake(emptyList(), boom = true)).redact(buf, w, h, 4, "k4").dropped)
        assertArrayEquals(before, buf)
    }

    @Test fun writesRedactionJsonWithEntries() {
        val dir = File(System.getProperty("java.io.tmpdir"), "ck-redact-${System.nanoTime()}").also { it.mkdirs() }
        try {
            val red = Redactor(Fake(listOf(Region(10, 10, 20, 20))), dir, padFraction = 0.0)
            red.redact(frame(), w, h, 7, "keyframes/000007.jpg")
            assertTrue(red.writeJson(dir))
            val t = File(dir, "privacy/redaction.json").readText()
            assertTrue(t.contains("\"mode\":\"faces\"") && t.contains("\"best_effort\":true"))
            assertTrue(t.contains("\"id\":7") && t.contains("[10,10,20,20]") && t.contains("\"status\":\"ok\""))
            assertTrue(!File(dir, "privacy/redaction.json.tmp").exists())
        } finally { dir.deleteRecursively() }
    }

    @Test fun failClosedRecordsDroppedStatusInJson() {
        val red = Redactor(Fake(null))
        assertTrue(red.redact(frame(), w, h, 5, "k5").dropped)
        assertTrue(red.toJson().contains("\"status\":\"dropped_detector_failure\""))
    }

    @Test fun outOfRangeAndEmptyRegionsAreClampedOrDropped() {
        val before = frame(); val buf = before.copyOf()
        val regs = listOf(Region(-10, -10, 200, 200), Region(100, 100, 120, 120), Region(10, 10, 10, 10))
        val e = Redactor(Fake(regs), padFraction = 0.0).redact(buf, w, h, 6, "k6")
        assertEquals(listOf(Region(0, 0, 64, 48)), e.regions)   // fully-outside and zero-area rects (pad -> null) are dropped
        assertTrue(!e.dropped)
        assertEquals(Redactor.FILL_Y, buf[w * h - 1]); assertEquals(Redactor.FILL_C, buf[buf.size - 1])
    }

    @Test fun rewritesJsonEveryTenEntriesAndReplacesStaleTmp() {
        val dir = File(System.getProperty("java.io.tmpdir"), "ck-redact-${System.nanoTime()}").also { it.mkdirs() }
        try {
            File(dir, "privacy").mkdirs(); File(dir, "privacy/redaction.json.tmp").writeText("stale")
            val red = Redactor(Fake(emptyList()), dir)
            repeat(9) { red.redact(frame(), w, h, it, "k$it") }
            assertTrue(!File(dir, "privacy/redaction.json").exists())
            red.redact(frame(), w, h, 9, "k9")
            val f = File(dir, "privacy/redaction.json")
            assertTrue(f.exists() && f.readText().contains("\"id\":9"))
            assertTrue(!File(dir, "privacy/redaction.json.tmp").exists())
            repeat(10) { red.redact(frame(), w, h, 10 + it, "k") }   // second flush replaces the existing file
            assertTrue(f.readText().contains("\"id\":19"))
        } finally { dir.deleteRecursively() }
    }

    @Test fun consecutiveFailuresFireHookOncePerStreakAndSuccessResets() {
        var fired = 0
        var fail = true
        val det = object : FaceRegionDetector {
            override val name = "t"
            override fun detect(nv21: ByteArray, w: Int, h: Int): List<Region>? = if (fail) null else emptyList()
        }
        val red = Redactor(det, onFailing = { fired++ })
        repeat(4) { red.redact(frame(), w, h, it, "k") }; assertEquals(0, fired)
        red.redact(frame(), w, h, 4, "k"); assertEquals(1, fired)
        repeat(3) { red.redact(frame(), w, h, 5 + it, "k") }; assertEquals(1, fired)   // same streak: no repeat
        fail = false; red.redact(frame(), w, h, 9, "k")                                // success resets
        fail = true; repeat(4) { red.redact(frame(), w, h, 10 + it, "k") }; assertEquals(1, fired)
        red.redact(frame(), w, h, 14, "k"); assertEquals(2, fired)
    }

    @Test fun unrotateRoundTrips() {
        // base 8x4; mark pixel (1,0)..(3,2). Rotated CW 90 -> dims 4x8: rx = bh-1-by, ry = bx.
        val r = Redactor.unrotate(90, 4 - 2, 1, 4 - 0, 3, 8, 4)
        assertEquals(Region(1, 0, 3, 2), r)
        // CCW 270: rx = by, ry = bw-1-bx -> rect rx 0..2, ry 8-3..8-1
        assertEquals(Region(1, 0, 3, 2), Redactor.unrotate(270, 0, 5, 2, 7, 8, 4))
    }
}
