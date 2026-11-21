package dev.capturekit.capture

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import java.io.File
import java.io.FileNotFoundException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicLong
import org.tensorflow.lite.DataType
import org.tensorflow.lite.Interpreter

/** One 2D detection in stored-keyframe pixels. [cls] is a core ObjectClass id ([LabelMap]). */
data class DetectedBox(val cls: Int, val score: Float, val x0: Float, val y0: Float, val x1: Float, val y1: Float)

/** A keyframe JPEG ready on disk. [rotationDeg] = clockwise rotation that makes it upright ([BoxMapping.uprightRotation]). */
class DetectorJob(val frameId: Int, val tNs: Long, val file: File, val rotationDeg: Int)

/**
 * Where detections go: [SessionManager] forwards them to the C API (ingest_detections) and to its listener (camera-feed
 * boxes). Called on the detector thread; an empty list means "frame processed, nothing found".
 */
fun interface DetectionSink {
    fun onDetections(frameId: Int, tNs: Long, boxes: List<DetectedBox>)
}

/** Seam for the model so the worker is JVM-testable. Returns null on failure. Called on the detector thread only. */
interface KeyframeDetector : AutoCloseable {
    val name: String
    fun detect(job: DetectorJob): List<DetectedBox>?
    override fun close() {}
}

/**
 * Runs the detector off the AR callback on its own thread. [post] schedules a Runnable on that thread (a HandlerThread
 * in production, a manual queue in tests). [submit] never blocks: the queue holds [capacity] jobs and drops the OLDEST
 * when full, counting the drop (IngestStats.detectorDropped and [dropped]). After [GIVE_UP_AFTER] consecutive failures
 * the worker disables itself (a broken model must not burn the CPU).
 */
class DetectorWorker(
    private val detector: KeyframeDetector,
    private val sink: DetectionSink,
    private val stats: IngestStats? = null,
    capacity: Int = 2,
    private val post: (Runnable) -> Unit,
    private val onClose: () -> Unit = {},
    private val log: (String, Throwable?) -> Unit = { m, t -> if (t == null) Log.w(TAG, m) else Log.e(TAG, m, t) },
) : AutoCloseable {
    private val queue = DropOldestQueue<DetectorJob>(capacity)
    private val processedCount = AtomicLong()
    private val failedCount = AtomicLong()
    @Volatile private var closed = false
    @Volatile private var busy = false   // a job is being detected/delivered; set before the poll so there is no gap
    @Volatile var disabled = false
        private set
    private var failStreak = 0   // detector thread only

    val dropped: Long get() = queue.dropped
    val processed: Long get() = processedCount.get()
    val failures: Long get() = failedCount.get()

    /** Queues [job]; returns false when the worker is closed or disabled. Safe on any thread, never blocks. */
    fun submit(job: DetectorJob): Boolean {
        if (closed || disabled) return false
        if (queue.offer(job) != null) {
            stats?.detectorDropped?.incrementAndGet()
            log("detector queue full: dropped oldest job (total ${queue.dropped})", null)
        }
        post(Runnable { drainOne() })
        return true
    }

    /** One job per posted Runnable, so a long backlog never starves the thread's message queue. */
    private fun drainOne() {
        if (closed || disabled) return
        busy = true
        try { drainJob() } finally { busy = false }
    }

    private fun drainJob() {
        val job = queue.poll() ?: return
        val boxes = try { detector.detect(job) } catch (t: Throwable) { log("detector threw", t); null }
        if (boxes == null) {
            failedCount.incrementAndGet()
            if (++failStreak >= GIVE_UP_AFTER) {
                disabled = true
                queue.clear()
                log("detector failed $failStreak times in a row: disabled", null)
            }
            return
        }
        failStreak = 0
        processedCount.incrementAndGet()
        stats?.detectorRuns?.incrementAndGet()
        try { sink.onDetections(job.frameId, job.tNs, boxes) } catch (t: Throwable) { log("detection sink threw", t) }
    }

    /**
     * Blocks until the queue is empty and no job is running, or [timeoutMs] passes; true when idle. Call off the main
     * and AR threads (sleeps in 10 ms steps). finish() uses it so queued keyframe jobs are not lost at the end.
     */
    fun awaitIdle(timeoutMs: Long): Boolean {
        val end = System.nanoTime() + timeoutMs * 1_000_000L
        while (!(queue.size() == 0 && !busy)) {
            if (disabled) return true   // backlog was cleared and nothing will run
            if (System.nanoTime() >= end) return false
            try { Thread.sleep(10) } catch (_: InterruptedException) { Thread.currentThread().interrupt(); return false }
        }
        return true
    }

    /** Stops accepting work, drops the backlog and releases the model. Idempotent. */
    override fun close() {
        if (closed) return
        closed = true
        queue.clear()
        post(Runnable {
            try { detector.close() } catch (_: Throwable) {}
            onClose()
        })
    }

    companion object {
        private const val TAG = "CK/Detector"
        const val GIVE_UP_AFTER = 5

        /**
         * Builds the production worker, or null (detector stays off) when [enabled] is false or the model file is not in
         * the library assets (scripts/fetch_models.sh puts it there; it is not committed). Never throws.
         */
        fun create(context: Context, enabled: Boolean, sink: DetectionSink, stats: IngestStats? = null): DetectorWorker? {
            if (!enabled) return null
            val det = try { LiteRtKeyframeDetector.open(context) } catch (t: Throwable) { Log.e(TAG, "detector init failed", t); null }
            if (det == null) {
                Log.w(TAG, "detector off: model ${LiteRtKeyframeDetector.MODEL_ASSET} not available")
                return null
            }
            val thread = HandlerThread("ck-detector").also { it.start() }
            val handler = Handler(thread.looper)
            return DetectorWorker(det, sink, stats, post = { handler.post(it) }, onClose = { thread.quitSafely() })
        }
    }
}

/** Pure decoding of the EfficientDet-Lite0 post-processed outputs (unit-tested). */
object DetectionDecoder {
    /** Which output tensor holds what, resolved from the output shapes. */
    class Roles(val boxes: Int, val classes: Int, val scores: Int, val count: Int)

    /**
     * Shapes: boxes [1,N,4], two [1,N] (classes and scores), count [1] or [1,1]. The Lite0 file orders its outputs
     * boxes, classes, scores, count (verified by running it); [swapIfNeeded] fixes a different order from the values.
     */
    fun roles(shapes: List<IntArray>): Roles? {
        var boxes = -1
        var count = -1
        val vec = ArrayList<Int>()
        shapes.forEachIndexed { i, s ->
            when {
                s.size == 3 && s[0] == 1 && s[2] == 4 -> boxes = i
                s.size == 2 && s[0] == 1 && s[1] > 1 -> vec.add(i)
                (s.size == 1 && s[0] == 1) || (s.size == 2 && s[0] == 1 && s[1] == 1) -> count = i
            }
        }
        if (boxes < 0 || count < 0 || vec.size != 2) return null
        return Roles(boxes, vec[0], vec[1], count)
    }

    /** True when [classes] holds values <= 1 only while [scores] exceeds 1: the two arrays are in the wrong role order. */
    fun swapIfNeeded(classes: FloatArray, scores: FloatArray, n: Int): Boolean {
        val cMax = (0 until minOf(n, classes.size)).maxOfOrNull { classes[it] } ?: 0f
        val sMax = (0 until minOf(n, scores.size)).maxOfOrNull { scores[it] } ?: 0f
        return sMax > 1.0f && cMax <= 1.0f
    }

    /**
     * Boxes of one frame: [boxes] = n x (ymin, xmin, ymax, xmax) normalised to the UPRIGHT image. Keeps scores >=
     * [minScore] and classes known to [LabelMap], maps boxes to stored pixels and returns at most [maxResults]
     * (model order = best first). Malformed values (NaN, inverted or empty boxes) are skipped.
     */
    fun decode(
        boxes: FloatArray, classes: FloatArray, scores: FloatArray, count: Int,
        minScore: Float, rotationDeg: Int, storedW: Int, storedH: Int, maxResults: Int = 20,
    ): List<DetectedBox> {
        val n = minOf(count, scores.size, classes.size, boxes.size / 4)
        val out = ArrayList<DetectedBox>()
        for (i in 0 until n) {
            val s = scores[i]
            if (!(s >= minScore) || s > 1.0f) continue
            val c = classes[i]
            if (!c.isFinite()) continue
            val cls = LabelMap.coreClass(c.toInt())
            if (cls == LabelMap.UNKNOWN) continue
            val b = BoxMapping.toStored(boxes[4 * i], boxes[4 * i + 1], boxes[4 * i + 2], boxes[4 * i + 3], rotationDeg, storedW, storedH)
                ?: continue
            out.add(DetectedBox(cls, s, b.x0, b.y0, b.x1, b.y1))
            if (out.size >= maxResults) break
        }
        return out
    }
}

/**
 * EfficientDet-Lite0 (uint8 320x320 input, post-processed outputs) on the LiteRT CPU interpreter (ADR 0017). The JPEG
 * is decoded downscaled, rotated upright and squashed to the model size, so normalised boxes map straight back.
 * Single-threaded: [detect] runs on the detector thread only. Needs the device runtime: not unit-testable on the JVM.
 */
class LiteRtKeyframeDetector private constructor(
    private val interp: Interpreter,
    private val roles: DetectionDecoder.Roles,
    private val inW: Int,
    private val inH: Int,
    private val minScore: Float,
) : KeyframeDetector {
    override val name = "efficientdet_lite0_detection"
    private val input: ByteBuffer = ByteBuffer.allocateDirect(inW * inH * 3).order(ByteOrder.nativeOrder())
    private val pixels = IntArray(inW * inH)
    private val outBufs = HashMap<Int, Any>()
    private val outBytes = HashMap<Int, ByteBuffer>()
    private val n: Int = interp.getOutputTensor(roles.scores).shape()[1]
    private val boxes = FloatArray(n * 4)
    private val classes = FloatArray(n)
    private val scores = FloatArray(n)

    init {
        for (i in 0 until interp.outputTensorCount) {
            val b = ByteBuffer.allocateDirect(interp.getOutputTensor(i).numBytes()).order(ByteOrder.nativeOrder())
            outBytes[i] = b
            outBufs[i] = b
        }
    }

    override fun detect(job: DetectorJob): List<DetectedBox>? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(job.file.path, bounds)
        val sw = bounds.outWidth
        val sh = bounds.outHeight
        if (sw <= 0 || sh <= 0) return null
        var sample = 1
        while (sw / (sample * 2) >= inW && sh / (sample * 2) >= inH) sample *= 2
        val src = BitmapFactory.decodeFile(job.file.path, BitmapFactory.Options().apply { inSampleSize = sample }) ?: return null
        val rot = ((job.rotationDeg % 360) + 360) % 360
        val rw = if (rot == 90 || rot == 270) src.height else src.width
        val rh = if (rot == 90 || rot == 270) src.width else src.height
        val m = Matrix().apply { postRotate(rot.toFloat()); postScale(inW.toFloat() / rw, inH.toFloat() / rh) }
        val scaled: Bitmap = Bitmap.createBitmap(src, 0, 0, src.width, src.height, m, true)
        try {
            scaled.getPixels(pixels, 0, inW, 0, 0, inW, inH)
        } finally {
            if (scaled !== src) scaled.recycle()
            src.recycle()
        }
        input.rewind()
        for (p in pixels) {
            input.put((p shr 16).toByte())   // R
            input.put((p shr 8).toByte())    // G
            input.put(p.toByte())            // B
        }
        input.rewind()
        for (b in outBytes.values) b.rewind()
        interp.runForMultipleInputsOutputs(arrayOf<Any>(input), outBufs)
        readFloats(outBytes.getValue(roles.boxes), boxes)
        var cls = classes
        var sc = scores
        readFloats(outBytes.getValue(roles.classes), cls)
        readFloats(outBytes.getValue(roles.scores), sc)
        val cnt = FloatArray(1).also { readFloats(outBytes.getValue(roles.count), it) }[0].toInt().coerceIn(0, n)
        if (DetectionDecoder.swapIfNeeded(cls, sc, cnt)) { val t = cls; cls = sc; sc = t }
        return DetectionDecoder.decode(boxes, cls, sc, cnt, minScore, job.rotationDeg, sw, sh)
    }

    private fun readFloats(b: ByteBuffer, dst: FloatArray) {
        b.rewind()
        b.asFloatBuffer().get(dst, 0, minOf(dst.size, b.remaining() / 4))
    }

    override fun close() { interp.close() }

    companion object {
        const val MODEL_ASSET = "efficientdet_lite0_detection.tflite"
        const val MIN_SCORE = 0.4f   // starting guess (hypothesis); tune on device recordings

        /**
         * True when the model file is packaged (scripts/fetch_models.sh puts it in the library assets; it is not
         * committed). Cheap (opens and closes the asset stream, reads nothing); never throws. The host uses it to decide
         * the session's understanding default before the session starts (ADR 0030).
         */
        fun available(context: Context): Boolean =
            try { context.assets.open(MODEL_ASSET).close(); true } catch (_: Throwable) { false }

        /** Opens the model from the library assets, or null when it is absent or has an unexpected layout. */
        fun open(context: Context, numThreads: Int = 2, minScore: Float = MIN_SCORE): LiteRtKeyframeDetector? {
            val bytes = try { context.assets.open(MODEL_ASSET).use { it.readBytes() } } catch (_: FileNotFoundException) { return null }
            val model = ByteBuffer.allocateDirect(bytes.size).order(ByteOrder.nativeOrder()).put(bytes).also { it.rewind() }
            val interp = Interpreter(model, Interpreter.Options().setNumThreads(numThreads))
            try {
                val t = interp.getInputTensor(0)
                val s = t.shape()   // [1, H, W, 3] uint8
                val roles = DetectionDecoder.roles((0 until interp.outputTensorCount).map { interp.getOutputTensor(it).shape() })
                val outputsOk = roles != null && (0 until interp.outputTensorCount).all { interp.getOutputTensor(it).dataType() == DataType.FLOAT32 }
                if (t.dataType() != DataType.UINT8 || s.size != 4 || s[3] != 3 || !outputsOk) {
                    Log.e("CK/Detector", "unexpected model layout: in ${t.dataType()} ${s.toList()}")
                    interp.close()
                    return null
                }
                return LiteRtKeyframeDetector(interp, roles!!, s[2], s[1], minScore)
            } catch (t: Throwable) {
                interp.close()
                throw t
            }
        }
    }
}
