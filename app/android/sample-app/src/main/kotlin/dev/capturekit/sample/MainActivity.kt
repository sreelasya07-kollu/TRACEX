package dev.capturekit.sample

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.app.ApplicationExitInfo
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import com.google.ar.core.ArCoreApk
import dev.capturekit.capture.SessionManager
import dev.capturekit.consent.ConsentStore

/** CaptureKit entry point: onboarding, scan readiness, and links to the user's scans. */
class MainActivity : Activity() {
    private var home = false
    private val ui = Handler(Looper.getMainLooper())
    private val prefs by lazy { getSharedPreferences("ck_prefs", MODE_PRIVATE) }
    private val consent by lazy { ConsentStore.get(this) }

    // Dynamic UI elements
    private var readyBadge: TextView? = null
    private var permCard: LinearLayout? = null
    private var arCard: LinearLayout? = null
    private var arCardText: TextView? = null
    private var arCardBtn: Button? = null
    private var recoveryCard: LinearLayout? = null
    private var recoveryText: TextView? = null
    private var consentSummaryText: TextView? = null
    private var voiceSwitch: Switch? = null
    @Volatile private var recoveryNote: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.decorView.setBackgroundColor(UiKit.BG)
        Features.prefetch(this)   // ADR 0032: debug-build switches read off the main thread before the capture screen
        if (prefs.getBoolean(KEY_ONBOARDED, false)) showHome() else showOnboarding(0)
        if (savedInstanceState == null) checkLastExit()
    }

    private fun checkLastExit() {
        Thread {
            val reason = lastExitReason()
            val recovered = try { SessionManager.recoverInterrupted(this) } catch (t: Throwable) { Log.w(TAG, "recovery failed", t); emptyList() }
            if (reason != null) Log.i(TAG, "last process exit: $reason; recovered ${recovered.size} scan(s)")
            if (recovered.isEmpty()) return@Thread
            runOnUiThread {
                recoveryNote = "Your last scan ended unexpectedly. It was recovered and saved."
                paintRecovery()
            }
        }.start()
    }

    private fun lastExitReason(): String? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return null
        return try {
            val am = getSystemService(ActivityManager::class.java) ?: return null
            val info = am.getHistoricalProcessExitReasons(packageName, 0, 1).firstOrNull() ?: return null
            if (info.timestamp <= prefs.getLong(KEY_EXIT_TS, 0L)) return null
            val text = "${exitName(info.reason)} status=${info.status} (${info.description ?: "-"})"
            prefs.edit().putLong(KEY_EXIT_TS, info.timestamp).putString(KEY_EXIT_REASON, text).apply()
            text
        } catch (t: Throwable) { Log.w(TAG, "exit reasons unavailable", t); null }
    }

    private fun exitName(r: Int) = when (r) {
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "CRASH_NATIVE"
        ApplicationExitInfo.REASON_CRASH -> "CRASH"
        ApplicationExitInfo.REASON_ANR -> "ANR"
        ApplicationExitInfo.REASON_SIGNALED -> "SIGNALED"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "LOW_MEMORY"
        ApplicationExitInfo.REASON_EXIT_SELF -> "EXIT_SELF"
        ApplicationExitInfo.REASON_USER_REQUESTED -> "USER_REQUESTED"
        ApplicationExitInfo.REASON_USER_STOPPED -> "USER_STOPPED"
        else -> "REASON_$r"
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun lp(bottom: Int = 12) = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        .apply { bottomMargin = dp(bottom) }
    private val wrap = ViewGroup.LayoutParams.WRAP_CONTENT

    private fun setScreen(root: View) {
        val scroll = ScrollView(this).apply {
            isFillViewport = true
            isVerticalScrollBarEnabled = false
            addView(root)
            UiKit.applyInsets(this, 20)
        }
        setContentView(UiKit.page(this, scroll))
    }

    // ---- onboarding (3 pages) ----
    private fun showOnboarding(page: Int) {
        home = false
        readyBadge = null; permCard = null; arCard = null; recoveryCard = null
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.BOTTOM }
        val last = 2
        val (title, lines) = when (page) {
            0 -> "Scan a room in 3D" to listOf(
                "CaptureKit guides you around the room, tracks what you have covered, and tells you what to revisit.",
                "Press Start and move smoothly.\nFollow the spoken and on-screen cues.\nFinish and review your scan.")
            1 -> "Camera and motion" to listOf(
                "The camera maps the room and tracks your movement. Your scan stays on this phone unless you choose to upload it.",
                "Motion sensors keep tracking stable. Uploads and anonymous telemetry are optional and off by default.")
            else -> "Before you start" to listOf(
                "Use good light.\nWalk slowly and keep the phone steady.\nTurn the volume up for spoken guidance.\nClose doors and keep people out of view.")
        }
        root.addView(View(this), LinearLayout.LayoutParams(1, 0, 1f))
        root.addView(UiKit.largeTitle(this, title), lp(12))
        for (l in lines) root.addView(UiKit.body(this, l, if (l == lines.first()) UiKit.LABEL_2 else UiKit.LABEL, 16f), lp(14))
        root.addView(View(this), LinearLayout.LayoutParams(1, 0, 1f))

        // Page dots
        root.addView(LinearLayout(this).apply {
            gravity = Gravity.CENTER
            contentDescription = "Step ${page + 1} of ${last + 1}"
            for (i in 0..last) addView(View(context).apply {
                background = UiKit.rounded(if (i == page) UiKit.LABEL else UiKit.LABEL_3, dp(4).toFloat())
            }, LinearLayout.LayoutParams(dp(if (i == page) 18 else 6), dp(6)).apply { marginStart = dp(3); marginEnd = dp(3) })
        }, lp(20))

        val next = UiKit.bigButton(this, if (page == last) (if (CameraPermission.granted(this)) "Continue" else "Allow camera and continue") else "Continue").apply {
            setOnClickListener {
                if (page < last) showOnboarding(page + 1)
                else if (CameraPermission.granted(this@MainActivity)) finishOnboarding()
                else CameraPermission.request(this@MainActivity)
            }
        }
        root.addView(next, lp(10))
        if (page > 0) root.addView(UiKit.secondaryButton(this, "Back").apply { setOnClickListener { showOnboarding(page - 1) } }, lp(0))
        setScreen(root)
    }

    private fun finishOnboarding() {
        prefs.edit().putBoolean(KEY_ONBOARDED, true).apply()
        showHome()
        if (!consent.hasConsent()) startActivity(Intent(this, ConsentActivity::class.java))
    }

    private fun openScans() = startActivity(Intent(this, ScansActivity::class.java))

    /** A glass card with a status-tinted title line, body text and an optional action. Hidden until needed. */
    private fun alertCard(kind: UiKit.Kind, title: String, bodyOut: (TextView) -> Unit, action: Button?): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = UiKit.card(this@MainActivity, 22)
            setPadding(dp(18), dp(16), dp(18), dp(16))
            visibility = View.GONE
            addView(UiKit.text(this@MainActivity, title, 16f, kind.color, UiKit.MEDIUM), lp(6))
            addView(UiKit.body(this@MainActivity, "", size = 14f).also(bodyOut), lp(if (action != null) 14 else 0))
            if (action != null) addView(action)
        }

    // ---- Home ----
    private fun showHome() {
        home = true
        val version = try { packageManager.getPackageInfo(packageName, 0).versionName } catch (t: Throwable) { "1.0" }
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        // 1. Large title + readiness line; scans shortcut on the right.
        root.addView(LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(4), dp(8), 0, 0)
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.VERTICAL
                addView(UiKit.largeTitle(this@MainActivity, "CaptureKit"))
                readyBadge = UiKit.text(this@MainActivity, "", 14f, UiKit.LABEL_2).also { addView(it, lp(0).apply { topMargin = dp(2) }) }
            }, LinearLayout.LayoutParams(0, wrap, 1f))
        }, lp(24))

        // 2. Alerts (each hidden unless needed).
        recoveryCard = alertCard(UiKit.Kind.OK, "Scan recovered", { recoveryText = it },
            UiKit.secondaryButton(this, "View scans").apply { setOnClickListener { openScans() } }).also { root.addView(it, lp(12)) }
        paintRecovery()
        permCard = alertCard(UiKit.Kind.ERROR, "Camera access needed",
            { it.text = "CaptureKit uses the camera to see the room and track your movement." },
            UiKit.bigButton(this, "Allow camera").apply { setOnClickListener { CameraPermission.retry(this@MainActivity) } })
            .also { root.addView(it, lp(12)) }
        arCardBtn = UiKit.bigButton(this, "Check again").apply { setOnClickListener { refresh() } }
        arCard = alertCard(UiKit.Kind.WARN, "Google Play Services for AR needed", { arCardText = it }, arCardBtn)
            .also { root.addView(it, lp(12)) }

        // 3. Hero: one clear action.
        root.addView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = UiKit.card(this@MainActivity, 30)
            setPadding(dp(22), dp(22), dp(22), dp(22))
            addView(UiKit.title(this@MainActivity, "Scan a room", 28f), lp(8))
            addView(UiKit.body(this@MainActivity, "Walk slowly around the room. CaptureKit tracks what you have covered and tells you what is missing."), lp(22))
            addView(UiKit.bigButton(this@MainActivity, "Start scan").apply {
                setOnClickListener {
                    if (!CameraPermission.granted(this@MainActivity)) {
                        CameraPermission.request(this@MainActivity)
                    } else if (!consent.hasConsent()) {
                        startActivity(Intent(this@MainActivity, ConsentActivity::class.java).putExtra(ConsentActivity.EXTRA_START_SCAN, true))
                    } else {
                        startActivity(Intent(this@MainActivity, CaptureActivity::class.java))
                    }
                }
            })
        }, lp(28))

        // 4. Library
        root.addView(UiKit.sectionHeader(this, "Library"))
        val voice = UiKit.toggle(this, SpeechRenderer.savedEnabled(this)).apply {
            contentDescription = "Voice guidance"
            setOnCheckedChangeListener { _, on -> SpeechRenderer.saveEnabled(this@MainActivity, on) }
        }
        voiceSwitch = voice
        root.addView(UiKit.group(this).apply {
            addView(UiKit.listRow(this@MainActivity, null, "My scans", "Saved rooms and their check results") { openScans() })
            addView(UiKit.listRow(this@MainActivity, null, "Voice guidance", "Spoken coaching while you scan", trailing = voice).apply {
                isClickable = true; foreground = UiKit.rowRipple(); setOnClickListener { voice.toggle() }
            })
        }, lp(28))

        // 5. Privacy
        root.addView(UiKit.sectionHeader(this, "Privacy"))
        root.addView(UiKit.group(this).apply {
            addView(UiKit.listRow(this@MainActivity, null, "Stored on this phone", "").also { row ->
                consentSummaryText = ((row.getChildAt(0) as LinearLayout).getChildAt(1) as TextView)
            })
            addView(UiKit.listRow(this@MainActivity, null, "Withdraw consent", titleColor = UiKit.RED, chevron = false) { confirmRevoke() })
        }, lp(28))

        // 6. Developer tools: debuggable builds only.
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) {
            root.addView(UiKit.sectionHeader(this, "Developer"))
            root.addView(UiKit.group(this).apply {
                addView(UiKit.listRow(this@MainActivity, null, "Sensor recorder", "Raw camera and IMU capture") {
                    startActivity(Intent(this@MainActivity, RecorderActivity::class.java))
                })
                addView(UiKit.listRow(this@MainActivity, null, "Clock check", "Camera and sensor timestamp offsets") {
                    startActivity(Intent(this@MainActivity, ClockCheckActivity::class.java))
                })
            }, lp(20))
        }

        root.addView(UiKit.caption(this, "Version $version").apply { gravity = Gravity.CENTER }, lp(8))

        setScreen(root)
        refresh()
    }

    private fun paintVoice() {
        val on = SpeechRenderer.savedEnabled(this)
        voiceSwitch?.let { if (it.isChecked != on) it.isChecked = on }
    }

    private fun paintRecovery() {
        val c = recoveryCard ?: return
        val n = recoveryNote
        c.visibility = if (n == null) View.GONE else View.VISIBLE
        if (n != null) recoveryText?.text = n
    }

    private fun paintConsent() {
        Telemetry.sync(this)
        val c = consent.current()
        consentSummaryText?.text = if (c == null) {
            "Consent not given yet. It is needed before scanning."
        } else {
            "Upload ${if (c.upload) "on" else "off"}  ·  Anonymous telemetry ${if (c.telemetry) "on" else "off"}"
        }
    }

    private fun confirmRevoke() {
        AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Withdraw consent?")
            .setMessage("Scanning will be blocked until you accept again. Uploading and anonymous telemetry stop, and recorded choices are removed from this phone. Your saved scans stay on the phone.")
            .setPositiveButton("Withdraw") { _, _ ->
                consent.revoke()
                dev.capturekit.upload.UploadWorker.cancelAll(this)
                paintConsent()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun refresh() {
        if (!home) return
        paintConsent()
        paintVoice()
        val ok = CameraPermission.granted(this)
        permCard?.visibility = if (ok) View.GONE else View.VISIBLE

        val av = try { ArCoreApk.getInstance().checkAvailability(this) } catch (t: Throwable) { null }
        val (readyText, isSupported) = when {
            av == null -> "AR status unknown" to false
            av.isTransient -> "Checking AR..." to false
            av == ArCoreApk.Availability.SUPPORTED_NOT_INSTALLED -> "Install Google Play Services for AR" to false
            av == ArCoreApk.Availability.SUPPORTED_APK_TOO_OLD -> "Update Google Play Services for AR" to false
            av.isSupported -> "Ready to scan" to true
            else -> "This phone does not support AR" to false
        }

        if (isSupported) {
            arCard?.visibility = View.GONE
        } else {
            arCard?.visibility = View.VISIBLE
            arCardText?.text = "$readyText. CaptureKit needs it to track the phone's position in the room."
        }

        readyBadge?.let {
            when {
                ok && isSupported -> { it.text = "Ready to scan"; it.setTextColor(UiKit.LABEL_2) }
                !ok -> { it.text = "Camera access needed"; it.setTextColor(UiKit.RED) }
                else -> { it.text = readyText; it.setTextColor(UiKit.AMBER) }
            }
        }

        if (av?.isTransient == true) ui.postDelayed({ refresh() }, 500)
    }

    override fun onResume() { super.onResume(); refresh() }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) UiKit.immersive(this)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        CameraPermission.onResult(this, grantResults)
        if (!home) finishOnboarding() else refresh()
    }

    override fun onPause() { ui.removeCallbacksAndMessages(null); super.onPause() }

    private companion object {
        const val KEY_ONBOARDED = "onboarded"
        const val KEY_EXIT_TS = "last_exit_ts"
        const val KEY_EXIT_REASON = "last_exit_reason"
        const val TAG = "CKMain"
    }
}
