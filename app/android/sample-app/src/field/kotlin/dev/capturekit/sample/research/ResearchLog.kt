package dev.capturekit.sample.research

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.os.Build
import android.text.InputType
import android.util.Log
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import dev.capturekit.capture.IngestStats
import dev.capturekit.capture.IntentCounter
import dev.capturekit.capture.ResearchReport
import dev.capturekit.capture.ThermalTrace
import dev.capturekit.capture.TrialArm
import dev.capturekit.capture.TrialSetup
import dev.capturekit.capture.VoiceStats
import dev.capturekit.sample.BuildConfig
import java.io.File
import java.util.concurrent.atomic.AtomicInteger

/**
 * Field flavour only. Trial setup (participant code, optional room label, arm guided|unguided) and the per-scan
 * research record, written by [ResearchReport] as `<bundle>.research.json` NEXT TO the bundle directory: never inside
 * it (bundle schema untouched) and never uploaded (uploads send the bundle directory only). The standard flavour has
 * a no-op class with the same API, so none of this ships in it. Methods other than [showSetup] are thread-safe.
 */
class ResearchLog(private val activity: Activity) {
    private val prefs = activity.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    @Volatile private var trial: TrialSetup? = load()

    private val instructions = AtomicInteger()
    private val thermal = ThermalTrace()
    private val intents = IntentCounter()
    @Volatile private var startMs = 0L
    @Volatile private var ingest: IngestStats? = null
    @Volatile private var voice: VoiceStats? = null

    val enabled: Boolean get() = true

    /** Unguided arm: the shell mutes voice and coaching; the core keeps recording and guiding internally. */
    val unguided: Boolean get() = trial?.arm == TrialArm.UNGUIDED

    /** HUD chip text. */
    val label: String? get() = trial?.let { "Trial ${it.participant} · ${it.arm.wire}" } ?: "Set trial"

    private fun load(): TrialSetup? {
        val code = TrialSetup.cleanCode(prefs.getString(K_CODE, null)) ?: return null
        return TrialSetup(code, TrialArm.fromWire(prefs.getString(K_ARM, null)), TrialSetup.cleanRoom(prefs.getString(K_ROOM, null)))
    }

    /** Main thread. Dialog stays open until the participant code is valid. */
    fun showSetup(onChanged: () -> Unit) {
        if (activity.isFinishing || activity.isDestroyed) return
        val cur = trial
        val d = activity.resources.displayMetrics.density
        fun dp(v: Int) = (v * d).toInt()
        val code = EditText(activity).apply {
            hint = "Participant code (A-Z, 0-9, - _), no names"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            setText(cur?.participant.orEmpty()); setSingleLine()
        }
        val room = EditText(activity).apply {
            hint = "Room label (optional)"; setText(cur?.room.orEmpty()); setSingleLine()
        }
        val guided = RadioButton(activity).apply { text = "Guided (voice and coaching on)"; id = 1 }
        val unguidedRb = RadioButton(activity).apply { text = "Unguided (voice and coaching off)"; id = 2 }
        val group = RadioGroup(activity).apply {
            addView(guided); addView(unguidedRb)
            check(if (cur?.arm == TrialArm.UNGUIDED) 2 else 1)
        }
        val err = TextView(activity).apply { setTextColor(0xFFC62828.toInt()); visibility = android.view.View.GONE }
        val body = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(8), dp(20), 0)
            val w = ViewGroup.LayoutParams.MATCH_PARENT
            addView(code, LinearLayout.LayoutParams(w, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(room, LinearLayout.LayoutParams(w, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(group); addView(err)
        }
        val dlg = AlertDialog.Builder(activity).setTitle("Trial setup").setView(body)
            .setPositiveButton("Save", null).setNegativeButton("Cancel", null).create()
        dlg.setOnShowListener {
            dlg.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val c = TrialSetup.cleanCode(code.text.toString())
                if (c == null) {
                    err.text = "Use 1 to 32 letters, digits, - or _ (no spaces, no names)."; err.visibility = android.view.View.VISIBLE
                    return@setOnClickListener
                }
                val t = TrialSetup(c, if (group.checkedRadioButtonId == 2) TrialArm.UNGUIDED else TrialArm.GUIDED, TrialSetup.cleanRoom(room.text.toString()))
                trial = t
                prefs.edit().putString(K_CODE, t.participant).putString(K_ARM, t.arm.wire).putString(K_ROOM, t.room).apply()
                dlg.dismiss()
                onChanged()
            }
        }
        dlg.show()
    }

    /** Session start (call right after the core session starts). */
    fun begin(ingest: IngestStats, voice: VoiceStats, nowMs: Long) {
        startMs = nowMs; this.ingest = ingest; this.voice = voice
        instructions.set(0)
        // fresh trace and counters per scan
        thermal.reset(); intents.reset()
        thermal.onLevel(ingest.policyLevel, 0)   // a scan that starts hot must not report max_level 0
    }

    fun onInstruction() { instructions.incrementAndGet() }
    fun onThermal(level: Int, nowMs: Long) = thermal.onLevel(level, (nowMs - startMs).coerceAtLeast(0))
    fun onIntent(name: String) = intents.add(name)

    /** After the core finished the bundle (any thread, does file I/O: not the main thread). Failure is logged, never thrown. */
    fun end(bundleDir: File, status: Int, keyframes: Int, durationMs: Long, nowMs: Long) {
        val ing = ingest ?: return
        val vs = voice ?: return
        try {
            val t = trial ?: TrialSetup("unset", TrialArm.GUIDED)
            val json = ResearchReport.toJson(ResearchReport.Input(
                bundleName = bundleDir.name, trial = t, device = "${Build.MANUFACTURER} ${Build.MODEL}", appVersion = BuildConfig.VERSION_NAME,
                durationMs = durationMs, keyframes = keyframes, finishStatus = status, ingest = ResearchReport.ingestMap(ing),
                thermalMaxLevel = thermal.maxLevel(), thermalChanges = thermal.changesSnapshot(),
                instructionsEmitted = instructions.get(), voice = vs.snapshot(nowMs), intents = intents.snapshot(),
            ))
            if (!ResearchReport.write(bundleDir, json)) Log.w(TAG, "research log not written for ${bundleDir.name}")
            else Log.i(TAG, "research log ${ResearchReport.fileFor(bundleDir).name} written")
        } catch (e: RuntimeException) {
            Log.w(TAG, "research log failed: ${e.javaClass.simpleName}")
        } finally { ingest = null; voice = null }
    }

    private companion object {
        const val TAG = "CK/Research"
        const val PREFS = "ck_research"
        const val K_CODE = "participant"; const val K_ARM = "arm"; const val K_ROOM = "room"
    }
}
