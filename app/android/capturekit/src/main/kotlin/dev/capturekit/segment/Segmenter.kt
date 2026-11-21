package dev.capturekit.segment

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import dev.capturekit.capture.DetectorScheduler
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * One camera CPU image (ARCore YUV_420_888, sensor orientation, never rotated) copied off the AR frame. Plane bytes are
 * the raw plane buffers: luma [y] with row stride [yRow]; chroma [u] / [v] with row stride [uvRow] and pixel stride [uvPix].
 */
class SegImage(
    val w: Int, val h: Int,
    val y: ByteArray, val yRow: Int,
    val u: ByteArray, val v: ByteArray, val uvRow: Int, val uvPix: Int,
) {
    /** Opaque ARGB of pixel ([px], [py]) (BT.601 full range, as the JPEG keyframes). Out-of-range indices are clamped. */
    fun argb(px: Int, py: Int): Int {
        val yi = (py * yRow + px).coerceIn(0, y.size - 1)
        val ci = (py / 2) * uvRow + (px / 2) * uvPix
        val yy = y[yi].toInt() and 0xFF
        val uu = (if (u.isEmpty()) 128 else u[ci.coerceIn(0, u.size - 1)].toInt() and 0xFF) - 128
        val vv = (if (v.isEmpty()) 128 else v[ci.coerceIn(0, v.size - 1)].toInt() and 0xFF) - 128
        val r = (yy + 1.402f * vv).toInt().coerceIn(0, 255)
        val g = (yy - 0.344136f * uu - 0.714136f * vv).toInt().coerceIn(0, 255)
        val b = (yy + 1.772f * uu).toInt().coerceIn(0, 255)
        return (0xFF shl 24) or (r shl 16) or (g shl 8) or b
    }
}

/** One model mask over the WHOLE camera image (the image is squashed to the model input): [bits] row-major, 1 = inside. */
class MaskBits(val w: Int, val h: Int, val bits: ByteArray, val score: Float)

/** A mask with its remembered track; ([cx], [cy]) = mask centroid, normalised 0..1 over the camera image (label anchor). */
class SegMask(val track: Track, val mask: MaskBits, val cx: Float, val cy: Float)

/** One segmentation run: the camera at capture time (for the pose warp between runs) and its masks. Never saved to the bundle. */
class SegResult(val tNs: Long, val camera: CameraSnapshot, val masks: List<SegMask>)

/** One run's input: the image, the camera it was taken with and the prompts projected into it. */
class SegJob(val tNs: Long, val image: SegImage, val camera: CameraSnapshot, val prompts: List<SegPrompt>)

/** Where results go (the overlay view). Called on the segmentation thread; must not block. */
fun interface SegmentationSink {
    fun onMasks(result: SegResult)
}

/**
 * Seam for the model (issue #8) so the worker is JVM-testable. Called on the segmentation thread only.
 * Returns one mask per prompt in the same order (null = nothing for that prompt), or null when the run failed.
 */
interface Segmenter : AutoCloseable {
    val name: String
    fun segment(image: SegImage, prompts: List<SegPrompt>): List<MaskBits?>?
    override fun close() {}
}

/**
 * Runs segmentation off the AR thread (issue #8; same pattern as DetectorScheduler / DetectorWorker). The GL thread asks
 * [shouldCapture] every frame (cheap): at most one run per [minIntervalNs] / ml_rate_scale of frame time (<= 2 Hz at
 * scale 1), only while tracking. Queue size 1: while a job is queued or running, a due frame and any [submit] are
 * dropped, counted ([dropped]) and logged (AGENTS.md B.8). The model is opened lazily on the worker by [open] (null =
 * model missing: the feature turns itself off); after [GIVE_UP_AFTER] consecutive failures the worker disables itself.
 * [TrackMemory] lives on the worker thread and gives every mask its stable track.
 * Threads: [shouldCapture] from ONE thread (the GL thread); [submit] / [close] from any thread; [post] runs on the worker.
 */
class SegmentationScheduler(
    private val open: () -> Segmenter?,
    private val sink: SegmentationSink,
    private val memory: TrackMemory = TrackMemory(),
    minIntervalNs: Long = DEFAULT_INTERVAL_NS,
    private val post: (Runnable) -> Unit,
    private val onClose: () -> Unit = {},
    private val log: (String, Throwable?) -> Unit = { m, t -> if (t == null) Log.w(TAG, m) else Log.e(TAG, m, t) },
    /** Called once, on the worker thread, when the worker turns itself off (model missing / failing). Must not block. */
    private val onDisabled: () -> Unit = {},
) : AutoCloseable {
    private val gate = DetectorScheduler(minIntervalNs)   // capture-thread only
    private val inFlight = AtomicBoolean(false)
    @Volatile private var pending: SegJob? = null
    private val droppedCount = AtomicLong()
    private val processedCount = AtomicLong()
    private val failedCount = AtomicLong()
    @Volatile private var closed = false
    @Volatile var disabled = false
        private set
    private var segmenter: Segmenter? = null   // worker thread only
    private var opened = false                 // worker thread only
    private var failStreak = 0                 // worker thread only

    val dropped: Long get() = droppedCount.get()
    val processed: Long get() = processedCount.get()
    val failures: Long get() = failedCount.get()
    val busy: Boolean get() = inFlight.get()

    /** True when the caller should copy this frame and [submit] it. A due frame while busy is a counted, logged drop. */
    fun shouldCapture(tNs: Long, mlRateScale: Double = 1.0, trackingNormal: Boolean = true): Boolean {
        if (closed || disabled) return false
        if (!gate.shouldRun(tNs, mlRateScale, false, trackingNormal)) return false
        if (inFlight.get()) { drop("busy at capture"); return false }
        return true
    }

    /** Queues [job] unless one is in flight (then the NEW job is dropped and counted). Never blocks. */
    fun submit(job: SegJob): Boolean {
        if (closed || disabled) return false
        if (!inFlight.compareAndSet(false, true)) { drop("busy at submit"); return false }
        pending = job
        post(Runnable { drainOne() })
        return true
    }

    private fun drop(where: String) {
        val n = droppedCount.incrementAndGet()
        log("segmentation $where: frame dropped (total $n)", null)
    }

    private fun drainOne() {
        val job = pending
        pending = null
        try { if (job != null && !closed && !disabled) runJob(job) } finally { inFlight.set(false) }
    }

    private fun runJob(job: SegJob) {
        if (!opened) {
            opened = true
            segmenter = try { open() } catch (t: Throwable) { log("segmenter init failed", t); null }
            if (segmenter == null) { log("segmentation off: model not available", null); disable(); return }
        }
        val seg = segmenter ?: return
        if (job.prompts.isEmpty()) {   // nothing to segment: no model run, the overlay clears
            processedCount.incrementAndGet()
            deliver(SegResult(job.tNs, job.camera, emptyList()))
            return
        }
        val masks = try { seg.segment(job.image, job.prompts) } catch (t: Throwable) { log("segmenter threw", t); null }
        if (masks == null || masks.size != job.prompts.size) {
            failedCount.incrementAndGet()
            if (++failStreak >= GIVE_UP_AFTER) { log("segmenter failed $failStreak times in a row: disabled", null); disable() }
            return
        }
        failStreak = 0
        processedCount.incrementAndGet()
        val tracks = memory.assign(job.prompts)
        val out = ArrayList<SegMask>(masks.size)
        for (i in masks.indices) {
            val m = masks[i] ?: continue
            val c = centroid(m) ?: continue
            out.add(SegMask(tracks[i], m, c.first, c.second))
        }
        deliver(SegResult(job.tNs, job.camera, out))
    }

    /** Worker thread: turns the feature off and tells the host once (it hides the overlay and its chip). */
    private fun disable() {
        if (disabled) return
        disabled = true
        try { onDisabled() } catch (t: Throwable) { log("onDisabled threw", t) }
    }

    private fun deliver(r: SegResult) { try { sink.onMasks(r) } catch (t: Throwable) { log("segmentation sink threw", t) } }

    /** Stops accepting work and releases the model on the worker. Idempotent. */
    override fun close() {
        if (closed) return
        closed = true
        pending = null
        post(Runnable {
            try { segmenter?.close() } catch (_: Throwable) {}
            segmenter = null
            onClose()
        })
    }

    companion object {
        private const val TAG = "CK/Segment"
        const val GIVE_UP_AFTER = 5
        /** <= 2 Hz at ml_rate_scale 1 (the SAM 2.1-tiny encoder alone is ~200 ms on a flagship GPU). */
        const val DEFAULT_INTERVAL_NS = 500_000_000L

        /** Centroid of the set pixels, normalised 0..1; null for an empty or malformed mask. */
        fun centroid(m: MaskBits): Pair<Float, Float>? {
            if (m.w <= 0 || m.h <= 0 || m.bits.size < m.w * m.h) return null
            var sx = 0L; var sy = 0L; var n = 0L
            for (yy in 0 until m.h) for (xx in 0 until m.w) if (m.bits[yy * m.w + xx].toInt() != 0) { sx += xx; sy += yy; n++ }
            if (n == 0L) return null
            return ((sx.toFloat() / n + 0.5f) / m.w) to ((sy.toFloat() / n + 0.5f) / m.h)
        }

        /**
         * Production worker, or null (feature off) when [enabled] is false or the model files are not in the library
         * assets (scripts/fetch_models.sh --with-segmentation). Cheap: the model is loaded on the worker at the first run.
         */
        fun create(context: Context, enabled: Boolean, sink: SegmentationSink, onDisabled: () -> Unit = {}): SegmentationScheduler? {
            if (!enabled) return null
            if (!LiteRtSam2Segmenter.available(context)) {
                Log.w(TAG, "segmentation off: model files not packaged (${LiteRtSam2Segmenter.ENCODER_ASSET})")
                return null
            }
            val app = context.applicationContext
            val thread = HandlerThread("ck-segment").also { it.start() }
            val handler = Handler(thread.looper)
            return SegmentationScheduler({ LiteRtSam2Segmenter.open(app) }, sink, post = { handler.post(it) }, onClose = { thread.quitSafely() }, onDisabled = onDisabled)
        }
    }
}
