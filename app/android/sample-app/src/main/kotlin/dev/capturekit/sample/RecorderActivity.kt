package dev.capturekit.sample

import android.app.Activity
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import dev.capturekit.raw.RawRecorder
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Phase 0 raw recorder screen: Start/Stop writes a session folder under
 * getExternalFilesDir("sessions"). Pull with: adb pull /sdcard/Android/data/dev.capturekit.sample/files/sessions
 */
open class RecorderActivity : Activity(), SensorEventListener {
    protected open val clockCheck = false
    private lateinit var ar: ArHost
    private lateinit var hud: Hud
    private lateinit var sensors: SensorManager
    @Volatile protected var recorder: RawRecorder? = null
    @Volatile private var frames = 0L
    @Volatile private var trackState = "?"
    @Volatile private var trackFailure = "NONE"
    private var recStartMs = 0L
    private var arMessage: String? = null
    private val ui = Handler(Looper.getMainLooper())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        sensors = getSystemService(SENSOR_SERVICE) as SensorManager
        ar = ArHost(this) { frame ->
            trackState = frame.camera.trackingState.name
            trackFailure = frame.camera.trackingFailureReason.name
            val r = recorder ?: return@ArHost
            if (clockCheck) r.onClockSample("ar", frame.timestamp) else r.onArFrame(frame)
            frames++
        }
        ar.onError = { msg -> arMessage = msg; refresh() }
        hud = Hud(this)
        hud.setCoaching(if (clockCheck) "Clock check: press Start, wait a few seconds, then Stop" else "Raw recorder: press Start, then move slowly around the room")
        hud.button.setOnClickListener { if (recorder == null) start() else stop() }
        setContentView(FrameLayout(this).apply {
            addView(ar.view, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
            addView(hud.root, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        })
        if (!CameraPermission.granted(this)) CameraPermission.request(this)
    }

    private fun start() {
        val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val prefix = if (clockCheck) "clock" else "raw"
        val dir = File(getExternalFilesDir("sessions"), "${prefix}_$stamp")
        recorder = RawRecorder(dir).also { it.writeDevice() }
        frames = 0
        recStartMs = SystemClock.elapsedRealtime()
        for (type in intArrayOf(Sensor.TYPE_GYROSCOPE, Sensor.TYPE_ACCELEROMETER)) {
            sensors.getDefaultSensor(type)?.let { sensors.registerListener(this, it, IMU_PERIOD_US) }
        }
        hud.setResult(null)
        hud.setRecording(true)
        hud.setDetail("Recording started -> ${dir.name}")
        hud.haptic(false)
    }

    private fun stop() {
        sensors.unregisterListener(this)
        val r = recorder
        val n = frames
        recorder = null
        r?.close()
        hud.setRecording(false)
        hud.setDetail(null)
        hud.haptic(true)
        if (r != null) {
            hud.setResult("Saved: ${r.dir.name}\nframes $n\n${r.dir}", UiKit.Kind.OK)
            Thread {
                val size = try { UiKit.dirSizeBytes(r.dir) } catch (t: Throwable) { 0L }
                runOnUiThread { hud.setResult("Saved: ${r.dir.name}\nframes $n | ${UiKit.humanSize(size)}\n${r.dir}", UiKit.Kind.OK) }
            }.start()
        }
    }

    /** UI refresh at ~4 Hz while resumed. */
    private val tick = object : Runnable {
        override fun run() { ar.checkStall(); refresh(); ui.postDelayed(this, 250) }
    }

    private fun refresh() {
        hud.setTracking(trackState, trackFailure, ar.live() || ar.lastFrameMs == 0L || ar.state != ArState.READY)
        recorder?.let { r ->
            hud.setRecInfo(SystemClock.elapsedRealtime() - recStartMs, "frames $frames  dropped ${r.droppedCount}")
        }
        val am = arMessage
        when {
            am != null && ar.state == ArState.NO_PERMISSION ->
                hud.setBanner("Camera permission is needed to scan the room.", UiKit.Kind.ERROR, CameraPermission.retryLabel()) { CameraPermission.retry(this) }
            am != null && ar.state != ArState.READY ->
                hud.setBanner(am, UiKit.Kind.ERROR, "Retry") { arMessage = null; ar.retry(); refresh() }
            else -> hud.setBanner(null)
        }
    }

    override fun onSensorChanged(e: SensorEvent) {
        val r = recorder ?: return
        val type = if (e.sensor.type == Sensor.TYPE_GYROSCOPE) "gyro" else "accel"
        if (clockCheck) r.onClockSample(type, e.timestamp)
        else r.onImu(type, e.timestamp, e.values[0], e.values[1], e.values[2])
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    override fun onResume() {
        super.onResume()
        arMessage = null
        ar.resume()
        ui.post(tick)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) UiKit.immersive(this)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (CameraPermission.onResult(this, grantResults)) { arMessage = null; ar.resume() }
        else { arMessage = "Camera permission is needed to scan the room."; refresh() }
    }

    override fun onPause() {
        ui.removeCallbacks(tick)
        super.onPause()
        if (recorder != null) stop()
        ar.pause()
    }

    override fun onDestroy() {
        hud.release()
        ar.close()
        super.onDestroy()
    }

    private companion object {
        /** 200 Hz: the cap without HIGH_SAMPLING_RATE_SENSORS (targetSdk 31+). Spec needs >=100 Hz. */
        const val IMU_PERIOD_US = 5000
    }
}

/** Logs raw AR-frame and IMU timestamps beside elapsedRealtimeNanos (clock-notes.md). */
class ClockCheckActivity : RecorderActivity() {
    override val clockCheck = true
}
