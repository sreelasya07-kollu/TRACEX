package dev.capturekit.capture

import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

/**
 * Cheap diagnostic counters for the shell -> core feed (AtomicLong only; safe on the AR thread).
 * Pure JVM: formatting and the luma buffer sizing helper are unit-tested.
 */
class IngestStats {
    val arFrames = AtomicLong()          // ARCore frames seen by ARAdapter
    val trackingPoses = AtomicLong()     // poses with TrackingState.TRACKING
    val otherPoses = AtomicLong()        // PAUSED / STOPPED
    val frameCalls = AtomicLong()        // candidate frames reaching acquireCameraImage
    val framesIngested = AtomicLong()    // ingestFrame returned OK
    val lumaShort = AtomicLong()         // Y plane smaller than stride*height (old sizing bug)
    val onFrameThrows = AtomicLong()   // session.onFrame threw (caught in ARAdapter)
    val pointsIngested = AtomicLong()
    val planesIngested = AtomicLong()
    val keyframes = AtomicLong()         // keyframe requests from the core
    val glDraws = AtomicLong()           // GL onDrawFrame calls (written by the AR host)
    val arDupFrames = AtomicLong()       // duplicate timestamps that REACHED the adapter/core (must stay 0 after dedupe)
    val glSkippedDup = AtomicLong()      // draws whose Frame.timestamp repeated and were kept away from the core
    val ringBusy = AtomicLong()          // candidates dropped because every ring slot was pinned by the JPEG encoder
    val detectorDropped = AtomicLong()   // detector jobs evicted from the 2-deep drop-oldest queue (DetectorWorker)
    val detectorRuns = AtomicLong()      // detector frames processed
    @Volatile var candidateStride = 3    // current FrameRateControl stride (every Nth AR frame becomes a candidate)
    @Volatile var lastReason = "NONE"
    @Volatile var policyLevel = 0        // last governor policy level from the core (read-only for consumers; written by SessionManager)
    @Volatile var lastError = ""
    private val acquireFail = ConcurrentHashMap<String, AtomicLong>()
    private val status = ConcurrentHashMap<Int, AtomicLong>()

    fun acquireFailed(e: Throwable) { acquireFail.getOrPut(e.javaClass.simpleName) { AtomicLong() }.incrementAndGet(); lastError = "${e.javaClass.simpleName}: ${e.message}" }
    private var errorsAtLastInterval = 0L

    /** Call once per log interval, after logLine(): clears lastError if no new error happened since the last call. */
    fun endInterval() {
        val total = onFrameThrows.get() + acquireFail.values.sumOf { it.get() }
        if (total == errorsAtLastInterval) lastError = ""
        errorsAtLastInterval = total
    }

    fun ingestStatus(code: Int) { status.getOrPut(code) { AtomicLong() }.incrementAndGet() }
    fun pose(tracking: Boolean, reason: String) { if (tracking) trackingPoses.incrementAndGet() else otherPoses.incrementAndGet(); lastReason = reason }

    /** "trk 612/640 frm 190 kf 14 pts 33 pl 4" */
    fun hudLine(): String =
        "trk ${trackingPoses.get()}/${arFrames.get()} frm ${framesIngested.get()} kf ${keyframes.get()} pts ${pointsIngested.get()} pl ${planesIngested.get()}"

    /** "gl 1234 (60.1/s) dup 0 skipped 600" for the AR host's own periodic log. */
    fun glLine(windowMs: Long, drawsInWindow: Long): String =
        "gl ${glDraws.get()} (${perSecond(drawsInWindow, windowMs)}/s) dup ${arDupFrames.get()} skipped ${glSkippedDup.get()}"

    fun logLine(): String {
        val af = acquireFail.entries.joinToString(",") { "${it.key}=${it.value.get()}" }.ifEmpty { "-" }
        val st = status.entries.sortedBy { it.key }.joinToString(",") { "${it.key}=${it.value.get()}" }.ifEmpty { "-" }
        return "${hudLine()} | other ${otherPoses.get()} reason $lastReason | frameCalls ${frameCalls.get()} acqFail $af ingestSt $st throws ${onFrameThrows.get()} lumaShort ${lumaShort.get()} gl ${glDraws.get()} dup ${arDupFrames.get()} stride $candidateStride ringBusy ${ringBusy.get()}" +
            if (lastError.isEmpty()) "" else " lastErr $lastError"
    }

    companion object {
        /** The Y plane's buffer can be stride*(h-1)+w; the core wants stride*h bytes. */
        /** count per second with one decimal, locale-independent; 0.0 for a non-positive window. */
        fun perSecond(count: Long, windowMs: Long): String {
            if (windowMs <= 0) return "0.0"
            val tenths = (count * 10_000L + windowMs / 2) / windowMs
            return "${tenths / 10}.${tenths % 10}"
        }

        fun lumaCapacity(remaining: Int, stride: Int, height: Int) = maxOf(remaining, stride * height)
    }
}
