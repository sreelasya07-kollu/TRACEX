package dev.capturekit.verdict

import java.text.SimpleDateFormat
import java.util.Locale

/** Pure helpers for the "My scans" list: when a bundle was recorded, its optional room label, its badge text. */
object ScanInfo {
    /** Bundle folders are named scan_yyyyMMdd_HHmmss (device local time); falls back to [lastModified]. */
    fun timeMs(dirName: String, lastModified: Long): Long {
        val m = Regex("^scan_(\\d{8}_\\d{6})$").find(dirName) ?: return lastModified
        return try {
            SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).apply { isLenient = false }.parse(m.groupValues[1])?.time ?: lastModified
        } catch (e: java.text.ParseException) { lastModified }
    }

    /** "room" of the field research file beside the bundle (JSON string), blank -> null. */
    fun roomLabel(researchJson: String?): String? {
        val raw = researchJson?.let { Regex("\"room\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"").find(it)?.groupValues?.get(1) } ?: return null
        return raw.replace("\\\"", "\"").replace("\\\\", "\\").trim().ifEmpty { null }
    }

    /** Badge wording: colour comes from the verdict (PASS green, WARN amber, FAIL red, else grey), words always accompany it. */
    fun badgeText(verdict: String?): String = when (verdict) {
        "PASS" -> "Passed"
        "WARN" -> "Usable, with notes"
        "FAIL" -> "Failed"
        else -> "Not checked"
    }
}
