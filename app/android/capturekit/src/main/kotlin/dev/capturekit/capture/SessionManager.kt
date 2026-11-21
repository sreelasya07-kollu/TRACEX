package dev.capturekit.capture

import android.content.Context
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import com.google.ar.core.Frame
import dev.capturekit.nativebridge.CaptureKitNative as N
import dev.capturekit.nativebridge.Status
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

/**
 * Orchestrates one capture session over the C API. No capture logic lives here (it is all in the core):
 * this class owns the handle, feeds ingest calls, answers keyframe requests by encoding JPEGs on a worker,
 * and polls events. Every ingest returns immediately; a FULL status is a counted, logged drop.
 *
 * Threading: onPose/onFrame/... run on the AR (GL) thread, onGyro/onAccel on the sensor thread, JPEG
 * encoding on a single worker, event polling on its own handler thread.
 */
class SessionManager(
    private val context: Context,
    val dir: File,
    private val clockOffsetNs: Long = 0L,
    private val frameEveryN: Int = 3, // ARCore ~30 fps -> core candidates ~10 fps; doubled under pressure (FrameRateControl)
    private val listener: Listener? = null,
    private val faceDetector: FaceRegionDetector? = null,  // test seam only; redaction is on iff redaction_mode=faces
    private val detectorEnabled: Boolean = false,          // 2D object detector (ADR 0017); the sample app turns it on by default (ADR 0030)
    private val uprightRotation: () -> Int = { defaultUprightRotation(context) },  // clockwise degrees that turn a stored keyframe upright
) : DetectionSink {
    private var redactor: Redactor? = null
    /** Called on the ck-jpeg thread once per streak of consecutive detector failures. Not wired to the HUD yet. */
    @Volatile var onRedactionFailing: (() -> Unit)? = null
    interface Listener {
        fun onEvent(e: CoreEvent) {}
        fun onPolicy(p: Policy) {}
        fun onState(state: Int) {}
        /**
         * 2D boxes of one keyframe, in stored-keyframe (= ARCore CPU image) pixels, after they went to the core. Called
         * on the detector thread (never the AR or main thread); an empty list = "ran, saw nothing". Must not block.
         */
        fun onDetections(frameId: Int, tNs: Long, boxes: List<DetectedBox>) {}
    }

    /**
     * One preallocated ring slot: Y (direct, also the buffer handed to ingestFrame; the core copies it before
     * returning) plus U/V, kept for a possible keyframe request. Buffers are reallocated only on a size change.
     * Ownership is tracked by [slotRing]: the AR thread writes only a claimed (unpinned, id-cleared) slot, ck-jpeg
     * reads only a pinned one; publish/pin go through the same lock, so the writes are visible to the reader.
     */
    private class Slot {
        var w = 0; var h = 0; var yRow = 0; var uvRow = 0; var uvPix = 0
        var tNs = 0L
        var y: ByteBuffer = ByteBuffer.allocateDirect(0)
        var u = ByteArray(0); var v = ByteArray(0)
    }

    @Volatile private var handle = 0L       // 0 = closed; written only under nativeLock after start()
    private val ring = Array(RING) { Slot() }
    private val slotRing = SlotRing(RING)
    private val frameId = AtomicInteger(0)
    private var nv21 = ByteArray(0)          // ck-jpeg thread only
    @Volatile private var policyLevel = 0    // written by ck-events, read on the AR thread
    @Volatile private var mlRateScale = 1.0  // last governor policy, read by ck-jpeg for the detector schedule
    @Volatile private var mustPause = false
    @Volatile private var trackingNormal = false
    @Volatile private var detector: DetectorWorker? = null
    /** True while the 2D detector is running in this session (flag on and model loaded); false before start and after finish. */
    val detectorActive: Boolean get() = detector?.let { !it.disabled } ?: false
    private val detectorSchedule = DetectorScheduler()   // ck-jpeg thread only
    private val nativeLock = Any()               // every native call that uses `handle` holds it (short JNI calls); destroy takes it too
    private val finishing = java.util.concurrent.atomic.AtomicBoolean(false)   // set by finish(); abandon() returns early
    /**
     * Runs one short native call with the live handle, or returns [closed] when the session is closed. Holding
     * nativeLock keeps destroy() (finish/abandon, which also set handle=0 under it) from freeing the handle mid-call.
     * Only the JNI call itself runs under the lock (a few microseconds; ingestFrame copies one Y plane), never I/O.
     */
    private inline fun <T> nat(closed: T, call: (Long) -> T): T {
        if (handle == 0L) return closed   // fast path once closed: no lock on the AR thread
        synchronized(nativeLock) {
            val h = handle
            return if (h == 0L) closed else call(h)
        }
    }

    private val rate = FrameRateControl(baseStride = frameEveryN, maxStride = frameEveryN * 2)
    private val encoder = Executors.newSingleThreadExecutor { Thread(it, "ck-jpeg") }
    private var pollThread: HandlerThread? = null
    private var pollHandler: Handler? = null
    @Volatile private var polling = false
    val droppedAtEdge = AtomicInteger(0)
    val stats = IngestStats()
    private val db = CaptureDb.get(context).sessions()

    private fun mono() = SystemClock.elapsedRealtimeNanos() + clockOffsetNs

    fun start(extraConfig: Map<String, Any> = emptyMap()): Boolean {
        redactor = if (extraConfig["redaction_mode"] == "faces") Redactor(faceDetector ?: PlatformFaceDetector(), dir, onFailing = {
            Log.e(TAG, "face detector failed ${Redactor.FAILING_AFTER} keyframes in a row: keyframes are being dropped")
            onRedactionFailing?.invoke()
        }) else null
        val cfg = buildMap<String, Any> {
            put("platform", "android"); put("model", "${Build.MANUFACTURER} ${Build.MODEL}")
            put("os", "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
            put("clock_base", "elapsedRealtimeNanos"); put("clock_offset_ns", clockOffsetNs)
            putAll(extraConfig)
        }
        LiveSessions.add(dir.absolutePath)   // before the "recording" row exists: recovery must skip this bundle
        handle = N.create(dir.absolutePath, toJson(cfg))
        if (handle <= 0) { Log.e(TAG, "create failed: ${-handle}"); handle = 0; LiveSessions.remove(dir.absolutePath); return false }
        encoder.execute { db.upsert(SessionRow(dir.absolutePath, System.currentTimeMillis(), "recording")) }
        stats.candidateStride = rate.stride
        val st = N.start(handle, mono())
        if (st != Status.OK) {
            Log.e(TAG, "start failed: $st")
            N.destroy(handle); handle = 0
            LiveSessions.remove(dir.absolutePath)
            return false
        }
        // Empty-entries redaction.json up front: a force-killed session with 0 keyframes must still validate.
        redactor?.let { if (!it.writeJson(dir)) Log.e(TAG, "initial privacy/redaction.json not written") }
        detectorSchedule.reset()
        detector = DetectorWorker.create(context, detectorEnabled, this, stats)   // null: flag off or model absent
        polling = true
        pollThread = HandlerThread("ck-events").also { it.start() }
        pollHandler = Handler(pollThread!!.looper).also { it.post(pollTick) }
        return true
    }

    private val pollTick = object : Runnable {
        override fun run() {
            if (!polling || handle == 0L) return
            drainEvents()
            pollHandler?.postDelayed(this, 50)
        }
    }

    private fun drainEvents() {
        val text = nat("") { N.pollEvents(it) }
        if (text.isEmpty()) return
        for (e in EventParser.parse(text)) {
            when (e) {
                is CoreEvent.KeyframeRequest -> { stats.keyframes.incrementAndGet(); encoder.execute { encodeKeyframe(e.frameId, e.file) } }
                is CoreEvent.StateChange -> listener?.onState(nat(0) { N.state(it) })
                else -> {}
            }
            listener?.onEvent(e)
        }
        val p = Policy.from(nat<DoubleArray?>(null) { N.policy(it) } ?: return)
        policyLevel = p.level
        mlRateScale = p.mlRateScale; mustPause = p.mustPause
        stats.policyLevel = p.level   // last known governor level, read by the field research log
        listener?.onPolicy(p)
    }

    // ---- ingest (never block) ----
    private fun note(st: Int, what: String) {
        if (st == Status.FULL) { val n = droppedAtEdge.incrementAndGet(); if (n % 100 == 1) Log.w(TAG, "core queue full: $what dropped ($n)") }
    }
    fun onPose(t: Long, x: Double, y: Double, z: Double, qw: Double, qx: Double, qy: Double, qz: Double, state: Int, reason: Int) {
        trackingNormal = state == 1
        note(nat(Status.STATE) { N.ingestPose(it, t, x, y, z, qw, qx, qy, qz, state, reason) }, "pose")
    }
    fun onIntrinsics(t: Long, w: Int, h: Int, fx: Double, fy: Double, cx: Double, cy: Double) {
        nat(Status.STATE) { N.ingestIntrinsics(it, t, w, h, fx, fy, cx, cy) }
    }
    fun onGyro(t: Long, x: Double, y: Double, z: Double) { note(nat(Status.STATE) { N.ingestGyro(it, t, x, y, z) }, "gyro") }
    fun onAccel(t: Long, x: Double, y: Double, z: Double) { note(nat(Status.STATE) { N.ingestAccel(it, t, x, y, z) }, "accel") }
    fun onPoints(t: Long, xyz: FloatArray, n: Int) { note(nat(Status.STATE) { N.ingestPoints(it, t, xyz, n) }, "points") }
    fun onPlane(t: Long, id: String, kind: String, d: DoubleArray) { nat(Status.STATE) { N.ingestPlane(it, t, id, kind, d) } }
    fun onPlatform(t: Long, thermal: Int, lowPower: Boolean, battery: Double, charging: Boolean, frameTimeMs: Double) {
        nat(Status.STATE) { N.ingestPlatform(it, t, thermal, lowPower, battery, charging, frameTimeMs) }
    }

    /**
     * Called by ARAdapter for every new ARCore frame; forwards every [FrameRateControl.stride]-th as a quality
     * candidate. Allocation-free in steady state: Y is copied once into a preallocated ring slot whose direct
     * buffer is what ingestFrame reads; U/V go into the same slot for a possible keyframe request.
     */
    fun onFrame(frame: Frame, t: Long) {
        if (handle == 0L || !rate.take()) return
        stats.frameCalls.incrementAndGet()
        val img = try { frame.acquireCameraImage() } catch (e: Exception) { stats.acquireFailed(e); adjustRate(t, false); return }
        var full = false
        img.use {
            val w = it.width; val h = it.height
            val yp = it.planes[0]; val up = it.planes[1]; val vp = it.planes[2]
            val ys = yp.buffer; val ysz = ys.remaining()
            val need = IngestStats.lumaCapacity(ysz, yp.rowStride, h)
            if (need > ysz) stats.lumaShort.incrementAndGet()   // old sizing would have returned CK_ERR_ARG
            val idx = slotRing.claim()
            if (idx == null) {
                // Every slot is being encoded: deliberate, counted, logged drop (never wait on the AR thread).
                val n = stats.ringBusy.incrementAndGet(); if (n % 100 == 1L) Log.w(TAG, "all $RING ring slots busy encoding: frame dropped ($n)")
                return@use
            }
            // Claimed: its id is cleared, so ck-jpeg cannot pin it while it is being written. Not OK -> not published,
            // and the next claim returns this same slot.
            val slot = ring[idx]
            if (slot.y.capacity() < need) slot.y = ByteBuffer.allocateDirect(need)
            slot.y.clear(); slot.y.put(ys.duplicate()); slot.y.flip()
            val id = frameId.getAndIncrement()
            val st = nat(Status.STATE) { N.ingestFrame(it, t, id, 0L, slot.y, w, h, yp.rowStride) }   // the core copies Y before returning
            stats.ingestStatus(st)
            if (st == Status.OK) stats.framesIngested.incrementAndGet()
            note(st, "frame")
            full = st == Status.FULL
            if (st != Status.OK) return@use
            val ub = up.buffer.duplicate(); val vb = vp.buffer.duplicate()
            if (slot.u.size != ub.remaining()) slot.u = ByteArray(ub.remaining())
            if (slot.v.size != vb.remaining()) slot.v = ByteArray(vb.remaining())
            ub.get(slot.u); vb.get(slot.v)
            slot.w = w; slot.h = h; slot.yRow = yp.rowStride; slot.uvRow = up.rowStride; slot.uvPix = up.pixelStride; slot.tNs = t
            slotRing.publish(idx, id)   // ck-jpeg may now pin it
        }
        adjustRate(t, full)
    }

    private fun adjustRate(t: Long, full: Boolean) {
        val changed = rate.update(t, full, policyLevel) ?: return
        stats.candidateStride = changed
        Log.i(TAG, "candidate stride -> every $changed frames (full=$full policy=$policyLevel)")
    }

    private fun encodeKeyframe(id: Int, rel: String) {
        val idx = slotRing.pin(id)
        if (idx == null) { nat(Status.STATE) { N.keyframeWritten(it, id, false) }; Log.w(TAG, "keyframe $id evicted before encoding"); return }
        val slot = ring[idx]
        try {
            val target = File(dir, rel); target.parentFile?.mkdirs()
            val tmp = File(dir, "$rel.tmp")
            val n = slot.w * slot.h * 3 / 2
            if (nv21.size != n) nv21 = ByteArray(n)
            toNv21(slot, nv21)
            val red = redactor?.redact(nv21, slot.w, slot.h, id, rel)   // in memory, before any pixel reaches disk
            if (red != null && red.dropped) {   // fail closed: never write a keyframe the detector could not check
                Log.w(TAG, "keyframe $id dropped: face detector failed"); nat(Status.STATE) { N.keyframeWritten(it, id, false) }; return
            }
            FileOutputStream(tmp).use { out ->
                YuvImage(nv21, ImageFormat.NV21, slot.w, slot.h, null).compressToJpeg(Rect(0, 0, slot.w, slot.h), JPEG_QUALITY, out)
                out.fd.sync()
            }
            val ok = tmp.renameTo(target)
            nat(Status.STATE) { N.keyframeWritten(it, id, ok) }
            if (ok) submitDetection(id, slot.tNs, target)
        } catch (t: Throwable) {
            Log.e(TAG, "keyframe encode failed", t); nat(Status.STATE) { N.keyframeWritten(it, id, false) }
        } finally {
            slotRing.unpin(idx)
        }
    }

    /** ck-jpeg thread only: queues the stored keyframe for the 2D detector when the schedule allows. Never the AR thread. */
    private fun submitDetection(id: Int, tNs: Long, file: File) {
        val d = detector ?: return
        if (!detectorSchedule.shouldRun(tNs, mlRateScale, mustPause, trackingNormal)) return
        d.submit(DetectorJob(id, tNs, file, uprightRotation()))
    }

    /** Detector thread: boxes of one keyframe (empty = ran, saw nothing) into the core. Full queue = counted drop. */
    override fun onDetections(frameId: Int, tNs: Long, boxes: List<DetectedBox>) {
        val flat = FloatArray(boxes.size * 6)
        boxes.forEachIndexed { i, b ->
            flat[6 * i] = b.cls.toFloat(); flat[6 * i + 1] = b.score
            flat[6 * i + 2] = b.x0; flat[6 * i + 3] = b.y0; flat[6 * i + 4] = b.x1; flat[6 * i + 5] = b.y1
        }
        note(nat(Status.STATE) { N.ingestDetections(it, tNs, frameId, flat, boxes.size) }, "detections")
        try { listener?.onDetections(frameId, tNs, boxes) } catch (t: Throwable) { Log.w(TAG, "detections listener threw", t) }
    }

    fun pause(): Int = nat(Status.STATE) { N.pause(it, mono()) }
    fun resume(): Int = nat(Status.STATE) { N.resume(it, mono()) }
    fun state(): Int = nat(0) { N.state(it) }
    fun policy(): Policy? = nat<DoubleArray?>(null) { N.policy(it) }?.let { Policy.from(it) }

    /** Stops capture, lets pending keyframes encode, finalizes the bundle. Returns the core status. */
    fun finish(): Int {
        if (handle == 0L || !finishing.compareAndSet(false, true)) return Status.STATE   // one finisher; abandon() backs off
        drainDetector()   // before stop: the core rejects detections once it is no longer capturing
        nat(Status.STATE) { N.stop(it, mono()) }
        Thread.sleep(300); drainEvents()   // final keyframe request
        encoder.shutdown()
        var idle = encoder.awaitTermination(10, java.util.concurrent.TimeUnit.SECONDS)
        if (!idle) {
            Log.e(TAG, "encoder did not drain in 10 s: shutdownNow")
            encoder.shutdownNow(); idle = encoder.awaitTermination(5, java.util.concurrent.TimeUnit.SECONDS)
            if (!idle) Log.e(TAG, "encoder STILL busy after shutdownNow; keyframes may be missing")
        }
        redactor?.let {   // only once the encoder is idle; before finalize hashes files; a failure must not stop finalize
            if (!idle) Log.e(TAG, "privacy/redaction.json NOT written: encoder busy")
            else if (!it.writeJson(dir)) Log.e(TAG, "privacy/redaction.json not written")
        }
        polling = false; pollThread?.quitSafely()
        // Long (hashes files): deliberately NOT under nativeLock so ingests (which now return STATE) never wait on it.
        // Only finish() frees the handle after this, so it stays valid here.
        val st = handle.let { h -> if (h == 0L) Status.STATE else N.finalizeSession(h) }
        db.setStatus(dir.absolutePath, if (st == Status.OK) "complete" else "unrecoverable")
        destroyHandle()
        LiveSessions.remove(dir.absolutePath)   // only after the final status is written
        return st
    }

    /** Frees the native session exactly once; handle becomes 0 under the lock, so no caller can see a freed handle. */
    private fun destroyHandle() {
        val h = synchronized(nativeLock) { val x = handle; handle = 0; x }   // no native call is in flight once we hold the lock
        if (h != 0L) N.destroy(h)
    }

    private fun closeDetector() { detector?.close(); detector = null }

    /** Gives queued detector jobs up to DETECTOR_DRAIN_MS to reach the core (finish() runs off the main thread), then closes it. */
    private fun drainDetector() {
        val d = detector ?: return
        if (!d.awaitIdle(DETECTOR_DRAIN_MS)) Log.w(TAG, "detector still busy after ${DETECTOR_DRAIN_MS} ms: pending jobs dropped")
        closeDetector()
    }

    /** Abandons the session without finalizing (simulates a kill; recovery picks it up on next launch). */
    fun abandon() {
        if (finishing.get()) return   // finish() owns the handle and destroys it itself
        polling = false
        closeDetector()
        destroyHandle()
        LiveSessions.remove(dir.absolutePath)
    }

    companion object {
        private const val TAG = "CKSession"
        private const val RING = 8
        private const val DETECTOR_DRAIN_MS = 500L
        private const val JPEG_QUALITY = 92
        /** Clockwise degrees that turn a stored (sensor-oriented) keyframe upright; 90 (portrait phone) if unknown. */
        @Suppress("DEPRECATION")
        fun defaultUprightRotation(context: Context): Int = try {
            val cm = context.getSystemService(Context.CAMERA_SERVICE) as android.hardware.camera2.CameraManager
            val back = cm.cameraIdList.firstOrNull {
                cm.getCameraCharacteristics(it).get(android.hardware.camera2.CameraCharacteristics.LENS_FACING) ==
                    android.hardware.camera2.CameraCharacteristics.LENS_FACING_BACK
            }
            val sensor = back?.let { cm.getCameraCharacteristics(it).get(android.hardware.camera2.CameraCharacteristics.SENSOR_ORIENTATION) } ?: 90
            val dm = context.getSystemService(Context.DISPLAY_SERVICE) as android.hardware.display.DisplayManager
            val display = when (dm.getDisplay(android.view.Display.DEFAULT_DISPLAY)?.rotation) {
                android.view.Surface.ROTATION_90 -> 90; android.view.Surface.ROTATION_180 -> 180
                android.view.Surface.ROTATION_270 -> 270; else -> 0
            }
            BoxMapping.uprightRotation(sensor, display)
        } catch (_: Throwable) { 90 }


        fun toJson(m: Map<String, Any>) = m.entries.joinToString(",", "{", "}") { (k, v) ->
            "\"$k\":" + if (v is Number || v is Boolean) v.toString() else "\"" + v.toString().replace("\\", "\\\\").replace("\"", "\\\"") + "\""
        }

        /**
         * App start: every session still marked "recording" and not owned by a live SessionManager in this process
         * was interrupted; recover it in the core.
         */
        @Synchronized   // Main and Capture both call this at launch: never recover the same bundle twice at once
        fun recoverInterrupted(context: Context): List<Pair<String, String>> {
            val dao = CaptureDb.get(context).sessions()
            val candidates = LiveSessions.recoverable(dao.interrupted().map { it.dir })
            return candidates.filter { dir ->
                // finish() removes its dir only after writing the final status: a dir that is not live (any more)
                // and still "recording" now was really interrupted.
                !LiveSessions.contains(dir) && dao.interrupted().any { it.dir == dir }
            }.map { dir ->
                val rep = N.recover(dir)
                dao.setStatus(dir, if (rep.contains("\"ok\":true")) "recovered" else "unrecoverable")
                dir to rep
            }
        }

        /** Slot planes -> NV21 in [out] (w*h*3/2 bytes); reads the slot's Y via a duplicate so its position is untouched. */
        private fun toNv21(s: Slot, out: ByteArray) {
            val y = s.y.duplicate()
            for (r in 0 until s.h) { y.position(r * s.yRow); y.get(out, r * s.w, s.w) }
            var o = s.w * s.h
            val cw = s.w / 2; val ch = s.h / 2
            for (r in 0 until ch) for (c in 0 until cw) {
                val i = r * s.uvRow + c * s.uvPix
                out[o++] = s.v[i]  // NV21 = Y plane, then interleaved V,U
                out[o++] = s.u[i]
            }
        }
    }
}
