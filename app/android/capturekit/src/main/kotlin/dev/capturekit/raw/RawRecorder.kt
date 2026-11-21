package dev.capturekit.raw

import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.media.Image
import android.os.Build
import android.os.SystemClock
import android.util.Log
import com.google.ar.core.Camera
import com.google.ar.core.Frame
import com.google.ar.core.Plane
import com.google.ar.core.TrackingFailureReason
import com.google.ar.core.TrackingState
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.Writer
import java.util.Locale
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicLong

/**
 * Phase 0 throwaway raw recorder (docs/program-spec/recording-protocol.md).
 *
 * All callbacks only enqueue; a single writer thread does file I/O and JPEG encoding.
 * The queue is bounded and every drop is counted and logged (AGENTS.md rule 8).
 */
class RawRecorder(
    val dir: File,
    private val clockOffsetNs: Long = 0L,
    private val keyframeIntervalNs: Long = 500_000_000L,
    queueCapacity: Int = 4096,
) {
    private val queue = ArrayBlockingQueue<() -> Unit>(queueCapacity)
    private val dropped = AtomicLong(0)
    @Volatile private var running = true
    private var lastFrameNs = Long.MIN_VALUE
    private var lastIntrinsics: String? = null
    private var frameIndex = 0
    private val writers = HashMap<String, Writer>()

    private val worker = Thread({
        while (running || queue.isNotEmpty()) {
            val job = queue.poll(50, java.util.concurrent.TimeUnit.MILLISECONDS) ?: continue
            try { job() } catch (t: Throwable) { Log.e(TAG, "write failed", t) }
        }
        writers.values.forEach { it.close() }
    }, "ck-raw-writer")

    init {
        File(dir, "frames").mkdirs()
        worker.start()
    }

    val droppedCount: Long get() = dropped.get()

    private fun enqueue(job: () -> Unit) {
        if (!queue.offer(job)) {
            val n = dropped.incrementAndGet()
            if (n % 100 == 1L) Log.w(TAG, "raw recorder queue full, dropped=$n")
        }
    }

    private fun line(name: String, json: String) = enqueue {
        val w = writers.getOrPut(name) { File(dir, name).bufferedWriter() }
        w.write(json); w.write("\n")
    }

    fun writeDevice(extra: Map<String, String> = emptyMap()) {
        val fields = linkedMapOf(
            "platform" to q("android"), "model" to q("${Build.MANUFACTURER} ${Build.MODEL}"),
            "os" to q("Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})"),
            "app_version" to q("0.0.1"), "pose_convention" to q("arcore"),
            "imu_frame" to q("android_device"), "clock_base" to q("elapsedRealtimeNanos"),
            "clock_offset_ns" to clockOffsetNs.toString(),
        )
        extra.forEach { (k, v) -> fields[k] = q(v) }
        val json = fields.entries.joinToString(",\n  ", "{\n  ", "\n}\n") { "\"${it.key}\": ${it.value}" }
        enqueue { File(dir, "device.json").writeText(json) }
    }

    /** Call from the GL/update thread after session.update(). Never blocks. */
    fun onArFrame(frame: Frame) {
        val t = frame.timestamp + clockOffsetNs
        val cam: Camera = frame.camera
        val p = cam.pose // sensor-oriented pose (conventions.md); ARCore quaternion is x,y,z,w
        val (state, reason) = mapTracking(cam.trackingState, cam.trackingFailureReason)
        line("poses.jsonl", String.format(Locale.US,
            "{\"t_ns\":%d,\"p\":[%.6f,%.6f,%.6f],\"q\":[%.8f,%.8f,%.8f,%.8f],\"tracking\":\"%s\",\"reason\":\"%s\"}",
            t, p.tx(), p.ty(), p.tz(), p.qw(), p.qx(), p.qy(), p.qz(), state, reason))

        val k = cam.imageIntrinsics
        val dims = k.imageDimensions
        val f = k.focalLength
        val c = k.principalPoint
        val intr = String.format(Locale.US, "\"w\":%d,\"h\":%d,\"fx\":%.4f,\"fy\":%.4f,\"cx\":%.4f,\"cy\":%.4f",
            dims[0], dims[1], f[0], f[1], c[0], c[1])
        if (intr != lastIntrinsics) {
            lastIntrinsics = intr
            line("intrinsics.jsonl", "{\"t_ns\":$t,$intr}")
        }

        if (cam.trackingState == TrackingState.TRACKING) {
            frame.acquirePointCloud().use { pc ->
                val b = pc.points
                if (b.remaining() >= 4) {
                    val sb = StringBuilder("{\"t_ns\":$t,\"pts\":[")
                    var first = true
                    while (b.remaining() >= 4) {
                        val x = b.get(); val y = b.get(); val z = b.get(); b.get()
                        if (!first) sb.append(',')
                        sb.append(String.format(Locale.US, "[%.4f,%.4f,%.4f]", x, y, z)); first = false
                    }
                    line("points.jsonl", sb.append("]}").toString())
                }
            }
            for (plane in frame.getUpdatedTrackables(Plane::class.java)) {
                if (plane.trackingState != TrackingState.TRACKING || plane.subsumedBy != null) continue
                val cp = plane.centerPose
                val n = cp.yAxis
                val u = cp.xAxis
                val kind = when (plane.type) {
                    Plane.Type.HORIZONTAL_UPWARD_FACING -> "horizontal_up"
                    Plane.Type.HORIZONTAL_DOWNWARD_FACING -> "horizontal_down"
                    else -> "vertical"
                }
                line("planes.jsonl", String.format(Locale.US,
                    "{\"t_ns\":%d,\"id\":%d,\"kind\":\"%s\",\"center\":[%.4f,%.4f,%.4f],\"normal\":[%.6f,%.6f,%.6f],\"axis_u\":[%.6f,%.6f,%.6f],\"extent\":[%.4f,%.4f]}",
                    t, System.identityHashCode(plane), kind, cp.tx(), cp.ty(), cp.tz(),
                    n[0], n[1], n[2], u[0], u[1], u[2], plane.extentX, plane.extentZ))
            }
        }

        if (t - lastFrameNs >= keyframeIntervalNs) {
            val img: Image = try { frame.acquireCameraImage() } catch (e: Exception) { return }
            lastFrameNs = t
            val nv21 = img.use { yuv420ToNv21(it) }
            val w = dims[0]; val h = dims[1]
            val i = frameIndex++
            enqueue {
                val name = String.format(Locale.US, "frames/%06d.jpg", i)
                FileOutputStream(File(dir, name)).use { out ->
                    YuvImage(nv21, ImageFormat.NV21, w, h, null).compressToJpeg(Rect(0, 0, w, h), 92, out)
                }
                val wr = writers.getOrPut("frames.jsonl") { File(dir, "frames.jsonl").bufferedWriter() }
                wr.write("{\"i\":$i,\"t_ns\":$t,\"file\":\"$name\",\"exposure_ns\":0}\n")
            }
        }
    }

    /** SensorEventListener.onSensorChanged; timestamp is the sensor event timestamp. */
    fun onImu(type: String, timestampNs: Long, x: Float, y: Float, z: Float) {
        line("imu.jsonl", String.format(Locale.US,
            "{\"t_ns\":%d,\"type\":\"%s\",\"v\":[%.6f,%.6f,%.6f]}", timestampNs + clockOffsetNs, type, x, y, z))
    }

    fun onClockSample(src: String, rawNs: Long) {
        val mono = SystemClock.elapsedRealtimeNanos()
        line("clockcheck.jsonl", "{\"src\":\"$src\",\"raw_ns\":$rawNs,\"mono_ns\":$mono}")
    }

    fun close() {
        running = false
        worker.join(5000)
        if (dropped.get() > 0) Log.w(TAG, "raw recorder finished with ${dropped.get()} dropped writes")
    }

    companion object {
        private const val TAG = "CKRawRecorder"
        private fun q(s: String) = "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\""

        fun mapTracking(state: TrackingState, reason: TrackingFailureReason): Pair<String, String> {
            val s = when (state) {
                TrackingState.TRACKING -> "NORMAL"
                TrackingState.PAUSED -> "LIMITED"
                TrackingState.STOPPED -> "NOT_AVAILABLE"
            }
            val r = when (reason) {
                TrackingFailureReason.NONE -> "NONE"
                TrackingFailureReason.BAD_STATE -> "BAD_STATE"
                TrackingFailureReason.INSUFFICIENT_LIGHT -> "INSUFFICIENT_LIGHT"
                TrackingFailureReason.EXCESSIVE_MOTION -> "EXCESSIVE_MOTION"
                TrackingFailureReason.INSUFFICIENT_FEATURES -> "INSUFFICIENT_FEATURES"
                TrackingFailureReason.CAMERA_UNAVAILABLE -> "CAMERA_UNAVAILABLE"
                else -> "OTHER"
            }
            return s to r
        }

        /** YUV_420_888 -> NV21, honouring row and pixel strides. */
        fun yuv420ToNv21(img: Image): ByteArray {
            val w = img.width; val h = img.height
            val out = ByteArray(w * h * 3 / 2)
            val y = img.planes[0]
            var o = 0
            for (r in 0 until h) {
                val buf = y.buffer
                for (c in 0 until w) out[o++] = buf.get(r * y.rowStride + c * y.pixelStride)
            }
            val u = img.planes[1]; val v = img.planes[2]
            for (r in 0 until h / 2) for (c in 0 until w / 2) {
                out[o++] = v.buffer.get(r * v.rowStride + c * v.pixelStride)
                out[o++] = u.buffer.get(r * u.rowStride + c * u.pixelStride)
            }
            return out
        }
    }
}
