package dev.capturekit.capture

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectorWorkerTest {
    private class Fake(val results: (DetectorJob) -> List<DetectedBox>?) : KeyframeDetector {
        override val name = "fake"
        val seen = ArrayList<Int>()
        var closed = false
        override fun detect(job: DetectorJob): List<DetectedBox>? {
            seen.add(job.frameId)
            return results(job)
        }
        override fun close() { closed = true }
    }

    private class Harness(det: KeyframeDetector, capacity: Int = 2) {
        val posted = ArrayDeque<Runnable>()
        val got = ArrayList<Triple<Int, Long, List<DetectedBox>>>()
        val logs = ArrayList<String>()
        val stats = IngestStats()
        var quit = false
        val w = DetectorWorker(
            det, { f, t, b -> got.add(Triple(f, t, b)) }, stats, capacity,
            post = { posted.addLast(it) }, onClose = { quit = true }, log = { m, _ -> logs.add(m) },
        )
        fun runAll() { while (posted.isNotEmpty()) posted.removeFirst().run() }
    }

    private fun job(id: Int) = DetectorJob(id, id * 1000L, File("kf_$id.jpg"), 90)
    private val box = DetectedBox(LabelMap.CHAIR, 0.8f, 1f, 2f, 3f, 4f)

    @Test fun submitNeverRunsInlineAndDeliversInOrder() {
        val h = Harness(Fake { listOf(box) })
        assertTrue(h.w.submit(job(1)))
        assertTrue(h.w.submit(job(2)))
        assertTrue(h.got.isEmpty())
        h.runAll()
        assertEquals(listOf(1, 2), h.got.map { it.first })
        assertEquals(2000L, h.got[1].second)
        assertEquals(2L, h.w.processed)
        assertEquals(2L, h.stats.detectorRuns.get())
    }

    @Test fun queueOfTwoDropsOldestAndCountsInStats() {
        val det = Fake { emptyList() }
        val h = Harness(det)
        for (i in 1..4) h.w.submit(job(i))
        assertEquals(2L, h.w.dropped)
        assertEquals(2L, h.stats.detectorDropped.get())
        assertEquals(2, h.logs.size)
        h.runAll()
        assertEquals(listOf(3, 4), det.seen)   // frames 1 and 2 were evicted
    }

    @Test fun emptyResultIsStillDelivered() {
        val h = Harness(Fake { emptyList() })
        h.w.submit(job(7))
        h.runAll()
        assertEquals(1, h.got.size)
        assertTrue(h.got[0].third.isEmpty())
    }

    @Test fun failuresAreCountedAndWorkerDisablesAfterFive() {
        val h = Harness(Fake { null })
        for (i in 1..5) { h.w.submit(job(i)); h.runAll() }
        assertEquals(5L, h.w.failures)
        assertTrue(h.w.disabled)
        assertFalse(h.w.submit(job(9)))
        assertTrue(h.got.isEmpty())
    }

    @Test fun successResetsTheFailureStreak() {
        var n = 0
        val h = Harness(Fake { if (++n % 2 == 1) null else emptyList() })
        for (i in 1..8) { h.w.submit(job(i)); h.runAll() }
        assertFalse(h.w.disabled)
        assertEquals(4L, h.w.failures)
        assertEquals(4L, h.w.processed)
    }

    @Test fun throwingDetectorDoesNotKillTheWorker() {
        val det = Fake { if (it.frameId == 1) throw IllegalStateException("boom") else listOf(box) }
        val h = Harness(det)
        h.w.submit(job(1))
        h.w.submit(job(2))
        h.runAll()
        assertEquals(1L, h.w.failures)
        assertEquals(listOf(2), h.got.map { it.first })
    }

    @Test fun closeStopsAcceptingDropsBacklogAndReleasesModel() {
        val det = Fake { emptyList() }
        val h = Harness(det)
        h.w.submit(job(1))
        h.w.close()
        h.w.close()
        assertFalse(h.w.submit(job(2)))
        h.runAll()
        assertTrue(det.seen.isEmpty())
        assertTrue(det.closed)
        assertTrue(h.quit)
    }
    @Test fun awaitIdleWaitsForQueuedJobsThenTimesOut() {
        val h = Harness(Fake { listOf(box) })
        assertTrue(h.w.awaitIdle(50))            // nothing queued
        h.w.submit(job(1))
        assertFalse(h.w.awaitIdle(30))           // queued, never run: times out
        val t = Thread { Thread.sleep(40); h.runAll() }
        t.start()
        assertTrue(h.w.awaitIdle(2000))          // drained by the other thread
        t.join()
        assertEquals(listOf(1), h.got.map { it.first })
    }
}

class DetectionDecoderTest {
    @Test fun labelMapKeepsOnlyCoreFurnitureClasses() {
        assertEquals(LabelMap.CHAIR, LabelMap.coreClass(61))
        assertEquals(LabelMap.COUCH, LabelMap.coreClass(62))
        assertEquals(LabelMap.POTTED_PLANT, LabelMap.coreClass(63))
        assertEquals(LabelMap.BED, LabelMap.coreClass(64))
        assertEquals(LabelMap.DINING_TABLE, LabelMap.coreClass(66))
        assertEquals(LabelMap.TV, LabelMap.coreClass(71))
        assertEquals(LabelMap.LAPTOP, LabelMap.coreClass(72))
        // ADR 0030: further COCO indoor classes (0-based index = COCO id - 1).
        assertEquals(LabelMap.BENCH, LabelMap.coreClass(14))
        assertEquals(LabelMap.TOILET, LabelMap.coreClass(69))
        assertEquals(LabelMap.OVEN, LabelMap.coreClass(78))
        assertEquals(LabelMap.SINK, LabelMap.coreClass(80))
        assertEquals(LabelMap.REFRIGERATOR, LabelMap.coreClass(81))
        // person, cat, remote, the never-predicted "???" slots (mirror 65, window 67, desk 68, door 70), microwave, toothbrush, junk
        for (c in intArrayOf(0, 16, 74, 65, 67, 68, 70, 77, 89, -1, 500)) assertEquals(LabelMap.UNKNOWN, LabelMap.coreClass(c))
        assertEquals("dining_table", LabelMap.coreName(LabelMap.DINING_TABLE))
        assertEquals("unknown", LabelMap.coreName(99))
    }

    /** Pinned against understanding.hpp kObjectClassNames (core test ObjectClassIdsAreFrozen pins the same table). */
    @Test fun coreNamesMatchTheCoreClassIds() {
        val expected = listOf(
            "unknown", "chair", "couch", "bed", "dining_table", "tv", "potted_plant", "laptop",
            "bench", "toilet", "sink", "refrigerator", "oven", "desk", "shelf", "cabinet", "lamp",
        )
        assertEquals(expected, LabelMap.coreNames)
        assertEquals("lamp", LabelMap.coreName(LabelMap.LAMP))
        assertEquals("refrigerator", LabelMap.coreName(LabelMap.REFRIGERATOR))
        // Every mapped COCO index lands on a real (non-unknown) core class.
        for (i in 0..90) { val c = LabelMap.coreClass(i); if (c != LabelMap.UNKNOWN) assertTrue(c in 1 until LabelMap.coreNames.size) }
    }

    @Test fun displayNamesMatchTheSpokenWords() {
        assertEquals("sofa", LabelMap.displayName("couch"))
        assertEquals("sofas", LabelMap.displayName("couch", plural = true))
        assertEquals("TV", LabelMap.displayName("tv"))
        assertEquals("table", LabelMap.displayName("dining_table"))
        assertEquals("fridge", LabelMap.displayName(LabelMap.REFRIGERATOR))
        assertEquals("shelves", LabelMap.displayName("shelf", plural = true))
        assertEquals("benches", LabelMap.displayName("bench", plural = true))
        assertEquals("door", LabelMap.displayName("door"))
        // Every core class has its own word (never the raw core name with an underscore).
        for (n in LabelMap.coreNames.drop(1)) assertFalse(n, LabelMap.displayName(n).contains('_'))
        // A class this build does not know (newer core) is shown, not dropped.
        assertEquals("coffee machine", LabelMap.displayName("coffee_machine"))
        assertEquals("coffee machines", LabelMap.displayName("coffee_machine", plural = true))
    }

    @Test fun boxLabelRoundsAndClampsTheScore() {
        assertEquals("chair 82%", LabelMap.boxLabel(LabelMap.CHAIR, 0.816f))
        assertEquals("sofa 100%", LabelMap.boxLabel(LabelMap.COUCH, 1.7f))
        assertEquals("TV 0%", LabelMap.boxLabel(LabelMap.TV, Float.NaN))
        assertEquals("unknown 40%", LabelMap.boxLabel(99, 0.4f))
    }

    @Test fun rolesFromLite0OutputShapes() {
        val r = DetectionDecoder.roles(listOf(intArrayOf(1, 25, 4), intArrayOf(1, 25), intArrayOf(1, 25), intArrayOf(1)))
        assertNotNull(r)
        assertEquals(0, r!!.boxes)
        assertEquals(1, r.classes)
        assertEquals(2, r.scores)
        assertEquals(3, r.count)
        assertNull(DetectionDecoder.roles(listOf(intArrayOf(1, 25, 4), intArrayOf(1, 25))))
    }

    @Test fun swapOnlyWhenRolesLookReversed() {
        assertFalse(DetectionDecoder.swapIfNeeded(floatArrayOf(62f, 16f), floatArrayOf(0.9f, 0.5f), 2))
        assertTrue(DetectionDecoder.swapIfNeeded(floatArrayOf(0.9f, 0.5f), floatArrayOf(62f, 16f), 2))
        assertFalse(DetectionDecoder.swapIfNeeded(floatArrayOf(0f, 0f), floatArrayOf(0.9f, 0.5f), 2))
    }

    @Test fun decodeFiltersMapsAndLimits() {
        // 5 candidates: chair 0.9, cat (not core) 0.8, couch 0.3 (below min), NaN box, tv 0.6
        val nan = Float.NaN
        val boxes = floatArrayOf(
            0.1f, 0.2f, 0.5f, 0.6f,
            0.1f, 0.1f, 0.2f, 0.2f,
            0.1f, 0.1f, 0.9f, 0.9f,
            nan, 0.1f, 0.2f, 0.3f,
            0.0f, 0.0f, 0.5f, 0.5f,
        )
        val classes = floatArrayOf(61f, 16f, 62f, 62f, 71f)
        val scores = floatArrayOf(0.9f, 0.8f, 0.3f, 0.7f, 0.6f)
        val out = DetectionDecoder.decode(boxes, classes, scores, 5, 0.4f, 0, 100, 200)
        assertEquals(listOf(LabelMap.CHAIR, LabelMap.TV), out.map { it.cls })
        assertEquals(20f, out[0].x0, 0.01f)   // xmin .2 * 100
        assertEquals(20f, out[0].y0, 0.01f)   // ymin .1 * 200
        assertEquals(60f, out[0].x1, 0.01f)
        assertEquals(100f, out[0].y1, 0.01f)
        assertEquals(1, DetectionDecoder.decode(boxes, classes, scores, 5, 0.4f, 0, 100, 200, maxResults = 1).size)
        assertEquals(1, DetectionDecoder.decode(boxes, classes, scores, 1, 0.4f, 0, 100, 200).size)   // count limits n
    }

    @Test fun decodeAppliesRotationToStoredPixels() {
        // upright box x .5..1.0, y 0..0.5; stored 640x480 rotated 90 cw -> stored x 0..320, y 0..240.
        val out = DetectionDecoder.decode(floatArrayOf(0f, 0.5f, 0.5f, 1f), floatArrayOf(62f), floatArrayOf(0.9f), 1, 0.4f, 90, 640, 480)
        assertEquals(1, out.size)
        val b = out[0]
        assertEquals(0f, b.x0, 0.01f)
        assertEquals(0f, b.y0, 0.01f)
        assertEquals(320f, b.x1, 0.01f)
        assertEquals(240f, b.y1, 0.01f)
    }
}
