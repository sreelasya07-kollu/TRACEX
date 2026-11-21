package dev.capturekit.sample.research

import android.app.Activity
import dev.capturekit.capture.IngestStats
import dev.capturekit.capture.VoiceStats
import java.io.File

/** Standard flavour: no research logging, no trial setup. Same API as the field implementation; every call is a no-op. */
@Suppress("UNUSED_PARAMETER")
class ResearchLog(activity: Activity) {
    val enabled: Boolean get() = false
    val unguided: Boolean get() = false
    val label: String? get() = null
    fun showSetup(onChanged: () -> Unit) {}
    fun begin(ingest: IngestStats, voice: VoiceStats, nowMs: Long) {}
    fun onInstruction() {}
    fun onThermal(level: Int, nowMs: Long) {}
    fun onIntent(name: String) {}
    fun end(bundleDir: File, status: Int, keyframes: Int, durationMs: Long, nowMs: Long) {}
}
