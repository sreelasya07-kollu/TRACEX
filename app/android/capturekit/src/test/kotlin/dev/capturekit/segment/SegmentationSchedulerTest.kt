package dev.capturekit.segment

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SegmentationSchedulerTest {
    private val posted = ArrayDeque<Runnable>()
    private val results = ArrayList<SegResult>()
    private val logs = ArrayList<String>()

    private fun runAll() { while (posted.isNotEmpty()) posted.removeFirst().run() }

    /** Fake model: one 2x2 mask per prompt (top-left pixel set), or null when [fail]. */
    private class FakeSegmenter(var fail: Boolean = false) : Segmenter {
        override val name = "fake"
        var runs = 0
        var closed = false
        var during: (() -> Unit)? = null
        override fun segment(image: SegImage, prompts: List<SegPrompt>): List<MaskBits?>? {
            runs++
            during?.invoke()
            if (fail) return null
            return prompts.map { MaskBits(2, 2, byteArrayOf(1, 0, 0, 0), 0.9f) }
        }
        override fun close() { closed = true }
    }

    private var disabledCalls = 0

    private fun scheduler(seg: Segmenter?, interval: Long = 500_000_000L) = SegmentationScheduler(
        open = { seg }, sink = { results.add(it) }, minIntervalNs = interval,
        post = { posted.addLast(it) }, log = { m, _ -> logs.add(m) }, onDisabled = { disabledCalls++ },
    )

    private val cam = CameraSnapshot(1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 100.0, 100.0, 2.0, 2.0, 4, 4)
    private val img = SegImage(4, 4, ByteArray(16), 4, ByteArray(8), ByteArray(8), 4, 2)
    private fun job(t: Long, vararg prompts: SegPrompt) = SegJob(t, img, cam, prompts.toList())
    private val chair = SegPrompt(TrackKey.CoreObject(3), SurfaceKind.OBJECT, "chair", 1f, 1f)

    @Test fun busyDropsTheNewJobAndCountsIt() {
        val s = scheduler(FakeSegmenter())
        assertTrue(s.submit(job(0, chair)))
        assertFalse(s.submit(job(1, chair)))            // one in flight (queue size 1): dropped
        assertEquals(1L, s.dropped)
        assertEquals(1, logs.count { it.contains("drop") })
        runAll()
        assertEquals(1L, s.processed)
        assertEquals(1, results.size)
        assertEquals(0L, results[0].tNs)                // the first job ran, the newer one was dropped
        assertTrue(s.submit(job(2, chair)))             // idle again: accepted
        runAll()
        assertEquals(2L, s.processed)
        assertEquals(1L, s.dropped)
    }

    @Test fun submitWhileTheModelRunsIsDropped() {
        val seg = FakeSegmenter()
        val s = scheduler(seg)
        var inner = true
        seg.during = { inner = s.submit(job(9, chair)) }
        assertTrue(s.submit(job(0, chair)))
        runAll()
        assertFalse(inner)
        assertEquals(1L, s.dropped)
        assertEquals(1, seg.runs)
    }

    @Test fun captureGateIsAtMostTwoHertzAndCountsBusyAsDrop() {
        val s = scheduler(FakeSegmenter())
        assertTrue(s.shouldCapture(0L))
        assertFalse(s.shouldCapture(100_000_000L))      // interval not over: not a drop
        assertEquals(0L, s.dropped)
        assertTrue(s.submit(job(0L, chair)))
        assertFalse(s.shouldCapture(600_000_000L))      // due but busy: dropped and counted
        assertEquals(1L, s.dropped)
        runAll()
        assertFalse(s.shouldCapture(700_000_000L))      // the busy drop restarted the interval
        assertTrue(s.shouldCapture(1_100_000_000L))
        assertFalse(s.shouldCapture(2_000_000_000L, trackingNormal = false))
        assertFalse(s.shouldCapture(3_000_000_000L, mlRateScale = 0.0))
        assertTrue(s.shouldCapture(4_000_000_000L, mlRateScale = 0.5))
        assertFalse(s.shouldCapture(4_600_000_000L, mlRateScale = 0.5))   // thermal scale 0.5 -> 1 s interval
    }

    @Test fun masksCarryStableTracks() {
        val s = scheduler(FakeSegmenter())
        val floor = SegPrompt(TrackKey.Plane(1), SurfaceKind.FLOOR, "Floor", 2f, 3f)
        s.submit(job(0, chair, floor)); runAll()
        s.submit(job(1, floor)); runAll()
        s.submit(job(2, floor, chair)); runAll()
        val first = results[0].masks.associate { it.track.label to it.track.id }
        val last = results[2].masks.associate { it.track.label to it.track.id }
        assertEquals(first, last)
        assertEquals(0.25f, results[0].masks[0].cx, 1e-6f)   // centroid of the top-left pixel of a 2x2 mask
        assertEquals(0.25f, results[0].masks[0].cy, 1e-6f)
    }

    @Test fun noPromptsSkipsTheModel() {
        val seg = FakeSegmenter()
        val s = scheduler(seg)
        s.submit(job(0)); runAll()
        assertEquals(0, seg.runs)
        assertEquals(1, results.size)
        assertTrue(results[0].masks.isEmpty())
    }

    @Test fun repeatedFailuresDisableTheWorker() {
        val s = scheduler(FakeSegmenter(fail = true))
        repeat(SegmentationScheduler.GIVE_UP_AFTER - 1) { assertTrue(s.submit(job(it.toLong(), chair))); runAll() }
        assertEquals(0, disabledCalls)
        assertTrue(s.submit(job(50, chair))); runAll()
        assertTrue(s.disabled)
        assertEquals(1, disabledCalls)                   // the host is told exactly once
        assertEquals(SegmentationScheduler.GIVE_UP_AFTER.toLong(), s.failures)
        assertFalse(s.submit(job(99, chair)))
        assertFalse(s.shouldCapture(10_000_000_000L))
        assertTrue(results.isEmpty())
    }

    @Test fun missingModelTurnsTheFeatureOff() {
        val s = scheduler(null)
        assertTrue(s.submit(job(0, chair)))
        runAll()
        assertTrue(s.disabled)
        assertEquals(1, disabledCalls)
        assertTrue(results.isEmpty())
        assertTrue(logs.any { it.contains("not available") })
    }

    @Test fun closeReleasesTheModelOnTheWorker() {
        val seg = FakeSegmenter()
        val s = scheduler(seg)
        s.submit(job(0, chair)); runAll()
        s.close()
        assertFalse(seg.closed)                          // released on the worker thread, not the caller's
        runAll()
        assertTrue(seg.closed)
        assertFalse(s.submit(job(1, chair)))
    }
}
