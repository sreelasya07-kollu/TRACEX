package dev.capturekit.sample

import android.app.Activity
import android.content.pm.PackageManager
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.os.SystemClock
import android.util.Log
import com.google.ar.core.ArCoreApk
import com.google.ar.core.CameraConfig
import com.google.ar.core.CameraConfigFilter
import com.google.ar.core.Config
import dev.capturekit.capture.IngestStats
import com.google.ar.core.Frame
import com.google.ar.core.Session
import com.google.ar.core.exceptions.CameraNotAvailableException
import com.google.ar.core.exceptions.SessionPausedException
import com.google.ar.core.exceptions.UnavailableApkTooOldException
import com.google.ar.core.exceptions.UnavailableArcoreNotInstalledException
import com.google.ar.core.exceptions.UnavailableDeviceNotCompatibleException
import com.google.ar.core.exceptions.UnavailableSdkTooOldException
import com.google.ar.core.exceptions.UnavailableUserDeclinedInstallationException
import java.util.EnumSet
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

enum class ArState { NO_PERMISSION, INSTALL_NEEDED, UNSUPPORTED, READY, ERROR }

/**
 * ARCore host: owns the Session and a GLSurfaceView whose renderer draws the camera image
 * (BackgroundRenderer) and hands each updated Frame to [onFrame]. session.update() and all GL work
 * run on the GL thread only; [onError] is always delivered on the UI thread. resume/pause/close are
 * idempotent and never throw: failures set [state]/[message] instead.
 */
class ArHost(private val activity: Activity, private val onFrame: (Frame) -> Unit) : GLSurfaceView.Renderer {
    /** Called on the UI thread with a user-readable message whenever [state] becomes an error state. */
    var onError: (String) -> Unit = {}

    @Volatile var state = ArState.NO_PERMISSION
        private set
    @Volatile var message = ""
        private set

    val view = GLSurfaceView(activity).apply {
        preserveEGLContextOnPause = true
        setEGLContextClientVersion(2)
        setRenderer(this@ArHost)
        renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
    }
    @Volatile var session: Session? = null
        private set

    private val background = BackgroundRenderer()
    private var installRequested = false
    private var resumed = false      // session.resume() succeeded and not yet paused
    private var viewResumed = false  // GLSurfaceView.onResume() called and not yet paused
    private var textureBound = false
    private var drawn = false
    /** Set before pause()/close() touch the Session; the GL thread stops using it at the top of the next draw. */
    @Volatile private var closing = false
    // TEMP A/B experiment switches (adb shell setprop debug.ck.legacy 1 / debug.ck.drain <ms>); remove after the crash work.
    private fun prop(n: String) = try { Runtime.getRuntime().exec(arrayOf("getprop", n)).inputStream.bufferedReader().readText().trim() } catch (t: Throwable) { "" }
    private val legacy = prop("debug.ck.legacy") == "1"
    /** Step 3b: delay session.pause() by this long after the GL thread stopped, so ARCore workers can drain. 0 = off. */
    private val drainMs = prop("debug.ck.drain").toLongOrNull() ?: 0L
    /** Step 5 A/B: debug.ck.blocking 1 = UpdateMode.BLOCKING (paces GL to the camera); default LATEST_CAMERA_IMAGE + dedupe. */
    private val blocking = prop("debug.ck.blocking") == "1"
    /** Step 8 spike: debug.ck.offmain 1 = view.onPause/session.pause/close run on the shared "ck-ar" thread (ARCore does not document this). */
    private val offMain = prop("debug.ck.offmain") == "1"
    private val mainHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private var pendingPause: Runnable? = null
    private var pendingDeadline = 0L
    /** True while a scan is recording: the session is never torn down/replaced then (a stall is only reported). */
    @Volatile var recording = false
    private var stallReported = false
    private var lastLoggedError: String? = null
    @Volatile var frameCallbackErrors = 0L
        private set
    /** elapsedRealtime of the last camera frame drawn (0 = none yet); used for the "Camera paused" chip and stall retry. */
    @Volatile var lastFrameMs = 0L
        private set
    private var resumedAtMs = 0L
    private var stallRetries = 0
    /** Draw/duplicate counters; CaptureActivity points this at the recording session's stats so they show in CK/Ingest. */
    @Volatile var stats = IngestStats()
    private var lastTs = 0L            // GL thread only
    private var statWindowStart = 0L   // GL thread only
    private var statWindowDraws = 0L   // GL thread only

    // Display geometry, written on the UI/GL threads, applied on the GL thread.
    @Volatile private var surfaceW = 0
    @Volatile private var surfaceH = 0
    @Volatile private var geometryDirty = true
    private var appliedRotation = -1

    private fun fail(newState: ArState, msg: String, t: Throwable? = null) {
        state = newState; message = msg
        if (t != null) Log.e(TAG, "Failed: $msg", t) else Log.w(TAG, msg)
        activity.runOnUiThread { onError(msg) }
    }

    fun resume() {
        if (resumed) return
        if (!awaitArQueue()) {
            // No failNow(): it would resume the GL view while ck-ar may still be pausing it. Retry calls resume() again.
            state = ArState.ERROR; message = "The camera is still shutting down. Wait a moment and retry."
            onError(message)
            return
        }
        flushPause()
        closing = false
        if (activity.checkSelfPermission(android.Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            state = ArState.NO_PERMISSION; message = "Camera permission is needed to scan the room."
            ensureView()
            onError(message)
            return
        }
        try {
            if (session == null) {
                if (ArCoreApk.getInstance().requestInstall(activity, !installRequested) == ArCoreApk.InstallStatus.INSTALL_REQUESTED) {
                    installRequested = true
                    state = ArState.INSTALL_NEEDED
                    message = "Google Play Services for AR is being installed. Return to the app when it finishes."
                    ensureView()
                    onError(message)
                    return
                }
                session = Session(activity).also { s ->
                    if (!legacy) selectCameraConfig(s)
                    s.configure(Config(s).apply {
                        focusMode = Config.FocusMode.AUTO
                        planeFindingMode = Config.PlaneFindingMode.HORIZONTAL_AND_VERTICAL
                        updateMode = if (blocking) Config.UpdateMode.BLOCKING else Config.UpdateMode.LATEST_CAMERA_IMAGE
                        // Features we never use: off, so ARCore does no work for them.
                        if (!legacy) lightEstimationMode = Config.LightEstimationMode.DISABLED
                        if (!legacy) instantPlacementMode = Config.InstantPlacementMode.DISABLED
                        if (!legacy) depthMode = Config.DepthMode.DISABLED
                    })
                }
                textureBound = false; geometryDirty = true; drawn = false
                logSessionInfo(session!!)
            }
            session?.resume()
            resumed = true
            resumedAtMs = SystemClock.elapsedRealtime()
            ensureView()
            state = ArState.READY; message = ""
        } catch (e: UnavailableUserDeclinedInstallationException) {
            failNow(ArState.INSTALL_NEEDED, "Google Play Services for AR is required. Install it to continue.", e)
        } catch (e: UnavailableArcoreNotInstalledException) {
            failNow(ArState.INSTALL_NEEDED, "Google Play Services for AR is not installed.", e)
        } catch (e: UnavailableApkTooOldException) {
            failNow(ArState.INSTALL_NEEDED, "Google Play Services for AR is out of date. Please update it.", e)
        } catch (e: UnavailableSdkTooOldException) {
            failNow(ArState.UNSUPPORTED, "This app is too old for the installed ARCore. Please update the app.", e)
        } catch (e: UnavailableDeviceNotCompatibleException) {
            failNow(ArState.UNSUPPORTED, "This phone does not support ARCore.", e)
        } catch (e: CameraNotAvailableException) {
            failNow(ArState.ERROR, "The camera is in use by another app. Close it and retry.", e)
        } catch (e: SecurityException) {
            failNow(ArState.NO_PERMISSION, "Camera permission is needed to scan the room.", e)
        } catch (t: Throwable) {
            failNow(ArState.ERROR, "AR could not start: ${t.javaClass.simpleName}", t)
        }
    }

    /** Failure path used from resume() (UI thread): keep the GL view running so the screen is not frozen. */
    private fun failNow(s: ArState, msg: String, t: Throwable) {
        state = s; message = msg
        Log.e(TAG, "Failed to start AR: $msg", t)
        ensureView()
        onError(msg)
    }

    /**
     * 30 fps, no depth sensor, no stereo; keeps the CPU image size ARCore picks today (no resolution change).
     * Must run before the first resume(). Failure keeps the default config.
     */
    private fun selectCameraConfig(s: Session) {
        try {
            val cur = s.cameraConfig
            val filter = CameraConfigFilter(s)
                .setTargetFps(EnumSet.of(CameraConfig.TargetFps.TARGET_FPS_30))
                .setDepthSensorUsage(EnumSet.of(CameraConfig.DepthSensorUsage.DO_NOT_USE))
                .setStereoCameraUsage(EnumSet.of(CameraConfig.StereoCameraUsage.DO_NOT_USE))
            val pick = s.getSupportedCameraConfigs(filter).firstOrNull {
                it.imageSize == cur.imageSize && it.facingDirection == CameraConfig.FacingDirection.BACK
            }
            if (pick == null) { Log.w(TAG, "no 30 fps config with cpu=${cur.imageSize}; keeping default"); return }
            if (pick.cameraId != cur.cameraId || pick.textureSize != cur.textureSize || pick.fpsRange != cur.fpsRange) {
                s.cameraConfig = pick
            }
            Log.i(TAG, "cameraConfig cpu=${s.cameraConfig.imageSize} fps=${s.cameraConfig.fpsRange} id=${s.cameraConfig.cameraId}")
        } catch (t: Throwable) { Log.w(TAG, "camera config selection failed; keeping default", t) }
    }

    private fun ensureView() { if (!viewResumed) { view.onResume(); viewResumed = true } }

    /** True when a camera frame was drawn within [withinMs]. */
    fun live(withinMs: Long = 1500L) = lastFrameMs != 0L && SystemClock.elapsedRealtime() - lastFrameMs < withinMs

    /**
     * Call from the UI tick. If the session is resumed but no frame arrived for 4 s (seen as a black preview right
     * after force-stop + relaunch), restart the session, at most twice in a row.
     */
    fun checkStall() {
        if (!resumed || state != ArState.READY) return
        if (live()) { stallRetries = 0; stallReported = false; return }
        val now = SystemClock.elapsedRealtime()
        if (recording) {
            // Replacing the Session mid-recording is unsafe (adapter state, ARCore worker threads): report only.
            if (now - maxOf(lastFrameMs, resumedAtMs) > 4000 && !stallReported) {
                stallReported = true
                Log.w(TAG, "no camera frame for 4 s while recording: NOT restarting the AR session")
            }
            return
        }
        if (now - maxOf(lastFrameMs, resumedAtMs) > 4000 && stallRetries < 2) {
            stallRetries++
            Log.w(TAG, "no camera frame for 4 s: restarting AR session (attempt $stallRetries)")
            retry()
        }
    }

    /** Tears the session down and tries again (used by the Retry button). */
    fun retry() {
        if (recording) { Log.w(TAG, "retry ignored while recording"); return }
        pause(); close(); resume()
    }

    fun pause() {
        closing = true
        if (offMain) {
            // Snapshot the main-thread flags, then let the shared ck-ar thread do the slow part; resume()/a new ArHost wait for it.
            val v = viewResumed; val r = resumed; val s = session   // s captured: pause runs before close even if main nulls session
            viewResumed = false; resumed = false
            AR_QUEUE.execute {
                var viewMs = -1L; var sessMs = -1L
                if (v) { val t0 = SystemClock.elapsedRealtime(); view.onPause(); viewMs = SystemClock.elapsedRealtime() - t0 }
                if (r) {
                    if (drainMs > 0) SystemClock.sleep(drainMs)
                    val t0 = SystemClock.elapsedRealtime()
                    try { s?.pause() } catch (t: Throwable) { Log.w(TAG, "pause failed", t) }
                    sessMs = SystemClock.elapsedRealtime() - t0
                }
                Log.i(TAG, "timing (ck-ar) view.onPause=${viewMs}ms session.pause=${sessMs}ms drain=${drainMs}ms")
            }
            return
        }
        var viewMs = -1L; var sessMs = -1L
        if (viewResumed) { val t0 = SystemClock.elapsedRealtime(); view.onPause(); viewResumed = false; viewMs = SystemClock.elapsedRealtime() - t0 }
        if (resumed) {
            resumed = false
            if (drainMs > 0) {
                // GL thread is stopped (no update/acquire from now on); give ARCore's workers time to finish the last image.
                val r = Runnable { pendingPause = null; sessionPause() }
                pendingPause = r; pendingDeadline = SystemClock.elapsedRealtime() + drainMs
                mainHandler.postDelayed(r, drainMs)
            } else sessMs = sessionPause()
        }
        Log.i(TAG, "timing view.onPause=${viewMs}ms session.pause=${sessMs}ms drain=${drainMs}ms")
    }

    private fun sessionPause(): Long {
        val t0 = SystemClock.elapsedRealtime()
        try { session?.pause() } catch (t: Throwable) { Log.w(TAG, "pause failed", t) }
        val ms = SystemClock.elapsedRealtime() - t0
        Log.i(TAG, "timing session.pause=${ms}ms")
        return ms
    }

    /** Runs a delayed session.pause() now (waiting out the rest of the drain) before resume()/close() touch the Session. */
    private fun flushPause() {
        val r = pendingPause ?: return
        mainHandler.removeCallbacks(r)
        val rem = pendingDeadline - SystemClock.elapsedRealtime()
        if (rem > 0) SystemClock.sleep(rem)
        pendingPause = null
        sessionPause()
    }

    /**
     * Main thread: waits (bounded) until every pause/close queued on ck-ar (by this or an earlier ArHost) has finished.
     * False on timeout/failure: the caller must not touch ARCore then.
     */
    private fun awaitArQueue(): Boolean {
        if (!offMain) return true
        val t0 = SystemClock.elapsedRealtime()
        val ok = try { AR_QUEUE.submit {}.get(AR_QUEUE_TIMEOUT_MS, java.util.concurrent.TimeUnit.MILLISECONDS); true }
        catch (t: java.util.concurrent.TimeoutException) { Log.e(TAG, "ck-ar still busy after ${AR_QUEUE_TIMEOUT_MS}ms (pause/close stuck): not resuming"); false }
        catch (t: Throwable) { Log.e(TAG, "ck-ar barrier failed: not resuming", t); false }
        val ms = SystemClock.elapsedRealtime() - t0
        if (ms > 0) Log.i(TAG, "timing resume waited ${ms}ms for ck-ar")
        return ok
    }

    fun close() {
        flushPause()
        closing = true
        if (offMain) {
            val s = session; session = null
            AR_QUEUE.execute {
                val t0 = SystemClock.elapsedRealtime()
                try { s?.close() } catch (t: Throwable) { Log.w(TAG, "close failed", t) }
                Log.i(TAG, "timing (ck-ar) session.close=${SystemClock.elapsedRealtime() - t0}ms")
            }
            return
        }
        val t0 = SystemClock.elapsedRealtime()
        try { session?.close() } catch (t: Throwable) { Log.w(TAG, "close failed", t) }
        session = null
        Log.i(TAG, "timing session.close=${SystemClock.elapsedRealtime() - t0}ms")
    }

    /** Once per Session: display refresh rate, current CPU image size and the supported camera configs. */
    @Suppress("DEPRECATION")
    private fun logSessionInfo(s: Session) {
        try {
            val hz = activity.windowManager.defaultDisplay.refreshRate
            val cur = s.cameraConfig
            Log.i(TAG, "display ${hz}Hz; updateMode=${if (blocking) "BLOCKING" else "LATEST_CAMERA_IMAGE"} offMain=$offMain; current cameraConfig cpu=${cur.imageSize} gpu=${cur.textureSize} fps=${cur.fpsRange} depth=${cur.depthSensorUsage} stereo=${cur.stereoCameraUsage} facing=${cur.facingDirection}")
            for (c in s.getSupportedCameraConfigs(CameraConfigFilter(s))) {
                Log.i(TAG, "  supported cpu=${c.imageSize} gpu=${c.textureSize} fps=${c.fpsRange} depth=${c.depthSensorUsage} stereo=${c.stereoCameraUsage} id=${c.cameraId}")
            }
        } catch (t: Throwable) { Log.w(TAG, "session info failed", t) }
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        background.init()
        textureBound = false
        geometryDirty = true
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
        surfaceW = width; surfaceH = height
        geometryDirty = true
    }

    @Suppress("DEPRECATION")
    override fun onDrawFrame(gl: GL10?) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        if (closing && !legacy) return
        val s = session ?: return
        try {
            if (!textureBound) { s.setCameraTextureName(background.textureId); textureBound = true }
            val rot = activity.windowManager.defaultDisplay.rotation
            if (rot != appliedRotation) { appliedRotation = rot; geometryDirty = true }
            if (geometryDirty && surfaceW > 0 && surfaceH > 0) {
                s.setDisplayGeometry(rot, surfaceW, surfaceH)
                geometryDirty = false
            }
            val frame = s.update()
            val fresh = countDraw(frame.timestamp)
            if (closing && !legacy) return
            // The background is redrawn every pass (the back buffer is undefined after a swap); only the core feed is deduped.
            if (frame.timestamp != 0L) { background.draw(frame); drawn = true; lastFrameMs = SystemClock.elapsedRealtime() }
            // LATEST_CAMERA_IMAGE returns the previous Frame when no new image arrived: do not feed it to the core again.
            if (drawn && (fresh || legacy)) {
                try { onFrame(frame) } catch (t: Throwable) {
                    // App/core adapter failure: not an AR failure, so do not touch [state]; log first and every 100th.
                    if (++frameCallbackErrors % 100 == 1L) Log.w(TAG, "onFrame callback failed ($frameCallbackErrors total)", t)
                }
            }
            lastLoggedError = null
        } catch (e: SessionPausedException) {
            // Session paused between resume() and the next frame; nothing to draw.
        } catch (e: CameraNotAvailableException) {
            reportRuntime(ArState.ERROR, "The camera became unavailable. Close other camera apps and retry.", e)
        } catch (t: Throwable) {
            reportRuntime(ArState.ERROR, "AR update error: ${t.javaClass.simpleName}", t)
        }
    }

    /** GL thread: count draws and repeated Frame timestamps; one CKArHost line every 2 s. */
    private fun countDraw(ts: Long): Boolean {
        stats.glDraws.incrementAndGet()
        val fresh = ts == 0L || ts != lastTs
        if (!fresh) stats.glSkippedDup.incrementAndGet()
        lastTs = ts
        val now = SystemClock.elapsedRealtime()
        if (statWindowStart == 0L) { statWindowStart = now; statWindowDraws = stats.glDraws.get() }
        if (now - statWindowStart >= 2000) {
            val draws = stats.glDraws.get()
            Log.i(TAG, stats.glLine(now - statWindowStart, draws - statWindowDraws))
            statWindowStart = now; statWindowDraws = draws
        }
        return fresh
    }

    private fun reportRuntime(s: ArState, msg: String, t: Throwable) {
        if (msg == lastLoggedError) return   // log and notify once per distinct failure, not per frame
        lastLoggedError = msg
        fail(s, msg, t)
    }

    private companion object {
        const val TAG = "CKArHost"
        const val AR_QUEUE_TIMEOUT_MS = 3000L   // below the 5 s input-dispatch ANR limit
        /** One process-wide thread so pause/close of one Session always finish before the next resume (R7: relaunch). */
        val AR_QUEUE: java.util.concurrent.ExecutorService =
            java.util.concurrent.Executors.newSingleThreadExecutor { r -> Thread(r, "ck-ar").apply { isDaemon = true } }
    }
}
