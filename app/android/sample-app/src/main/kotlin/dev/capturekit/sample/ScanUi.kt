package dev.capturekit.sample

import android.content.Context
import android.widget.TextView
import dev.capturekit.upload.UploadState
import dev.capturekit.upload.UploadStatus
import dev.capturekit.verdict.VerdictText

/** Wording and colours shared by the review screen and "My scans". Words always accompany colour. */
object ScanUi {
    private const val GREY = 0xFF546E7A.toInt()
    private const val BLUE = 0xFF1565C0.toInt()

    @Volatile private var cached: VerdictText? = null

    /** Reads assets/verdict_messages.json once (blocking disk read: call off the main thread). Never throws. */
    fun verdictText(ctx: Context): VerdictText = cached ?: synchronized(this) {
        cached ?: (try { ctx.applicationContext.assets.open("verdict_messages.json").use { VerdictText.parse(it.readBytes().decodeToString()) } }
        catch (t: Throwable) { VerdictText.FALLBACK }).also { cached = it }
    }

    fun verdictColor(verdict: String?): Int = when (verdict) {
        "PASS" -> UiKit.GREEN
        "WARN" -> UiKit.AMBER
        "FAIL" -> UiKit.RED
        else -> GREY
    }

    fun uploadText(s: UploadStatus, unmeteredOnly: Boolean = true): String = when (s.state) {
        UploadState.NONE -> "Not uploaded"
        UploadState.WAITING -> if (unmeteredOnly) "Upload waiting for Wi-Fi" else "Upload waiting for a network"
        UploadState.UPLOADING -> if (s.percent in 0..100) "Uploading ${s.percent}%" else "Uploading"
        UploadState.RETRYING -> "Upload will retry soon"
        UploadState.VERIFIED -> "Uploaded and verified"
        UploadState.FAILED -> "Upload failed" + (s.error?.takeIf { it.isNotBlank() }?.let { ": $it" } ?: "")
    }

    fun uploadColor(s: UploadStatus): Int = when (s.state) {
        UploadState.NONE -> GREY
        UploadState.WAITING -> GREY
        UploadState.UPLOADING -> BLUE
        UploadState.RETRYING -> UiKit.AMBER
        UploadState.VERIFIED -> UiKit.GREEN
        UploadState.FAILED -> UiKit.RED
    }

    fun paintUpload(chip: TextView, s: UploadStatus, unmeteredOnly: Boolean = true) {
        val t = uploadText(s, unmeteredOnly)
        UiKit.setChip(chip, t, uploadColor(s))
        chip.contentDescription = t.replace("%", " percent")
    }
}
