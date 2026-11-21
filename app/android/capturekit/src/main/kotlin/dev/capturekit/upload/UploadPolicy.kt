package dev.capturekit.upload

import androidx.work.WorkInfo
import java.io.File

/** What the user sees for one bundle's upload. NONE = never queued (or not allowed). */
enum class UploadState {
    NONE, WAITING, UPLOADING, RETRYING, VERIFIED, FAILED;

    companion object {
        /** WorkManager state -> user state. ENQUEUED after at least one attempt means the worker asked for a retry. */
        fun fromWork(state: WorkInfo.State, runAttempt: Int): UploadState = when (state) {
            WorkInfo.State.ENQUEUED -> if (runAttempt > 0) RETRYING else WAITING
            WorkInfo.State.BLOCKED -> WAITING
            WorkInfo.State.RUNNING -> UPLOADING
            WorkInfo.State.SUCCEEDED -> VERIFIED
            WorkInfo.State.FAILED, WorkInfo.State.CANCELLED -> FAILED
        }
    }
}

data class UploadStatus(val state: UploadState, val percent: Int = -1, val error: String? = null)

/** Pure (JVM-testable) upload gate: consent now AND a usable verdict. Checked at enqueue and again on every worker run. */
object UploadPolicy {
    enum class Decision { RUN, NO_CONSENT, VERDICT_FAIL, NO_VERDICT }

    /** Upload needs consent now and a PASS or WARN verdict; null/unparseable (not finalized) is not eligible. [consentNow] must come from ConsentStore at the moment of the call, so a revoke stops queued work. */
    fun decide(consentNow: Boolean, verdict: String?): Decision = when {
        !consentNow -> Decision.NO_CONSENT
        verdict == "FAIL" -> Decision.VERDICT_FAIL
        verdict != "PASS" && verdict != "WARN" -> Decision.NO_VERDICT   // no finalized manifest: nothing trustworthy to upload
        else -> Decision.RUN
    }

    fun eligible(consentNow: Boolean, verdict: String?) = decide(consentNow, verdict) == Decision.RUN

    /** validation.verdict of manifest.json text, or null when absent/unreadable (hand-parsed: org.json is stubbed in JVM tests). */
    fun verdictOf(manifestJson: String): String? =
        Regex("\"verdict\"\\s*:\\s*\"(PASS|WARN|FAIL)\"").find(manifestJson)?.groupValues?.get(1)

    /** Beside the bundle (not inside it, so the bundle's file list stays exactly what the core wrote). */
    fun verifiedMarker(bundle: File): File = File(bundle.absoluteFile.parentFile, bundle.name + ".upload_verified")

    fun isMarker(name: String) = name.endsWith(".upload_verified")

    fun percent(done: Long, total: Long): Int = if (total <= 0) 0 else ((done * 100) / total).toInt().coerceIn(0, 100)
}
