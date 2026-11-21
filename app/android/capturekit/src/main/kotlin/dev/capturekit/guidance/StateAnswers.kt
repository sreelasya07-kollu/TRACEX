package dev.capturekit.guidance

/** Questions the app can answer about the scan from a [ScanStateSnapshot] alone. */
enum class StateQuestion { MISSING, PROGRESS }

/**
 * A spoken/shown answer. [cites] are region names that exist in the snapshot ([ScanStateSnapshot.regions]);
 * empty when [abstained] or when the answer is a whole-room statement.
 */
data class Answer(val text: String, val abstained: Boolean, val cites: List<String> = emptyList())

/**
 * Fixed-template answers over the last overlay snapshot (ADR 0019). No LLM, no network, no guessing: if the
 * snapshot is missing, empty or tracking is lost, or the question is unknown, the answer is [ABSTAIN]. Every
 * region named in a sentence comes from the snapshot; walls are "Wall N".
 */
object StateAnswers {
    const val ABSTAIN = "I am not sure yet. Keep scanning."

    /** A wall below this percentage counts as not fully scanned (same threshold as the wall-tick haptic). */
    const val WALL_DONE_PCT = 80

    /** Completeness at or above this lets "what is missing" say the scan can be finished. */
    const val FINISH_COMPLETENESS = 0.9

    private const val MAX_LISTED = 3

    private val ABSTAINED = Answer(ABSTAIN, abstained = true)

    /** Voice shell entry: only [VoiceIntent.MISSING] is a state question. */
    fun questionFor(intent: VoiceIntent): StateQuestion? = if (intent == VoiceIntent.MISSING) StateQuestion.MISSING else null

    fun answer(question: StateQuestion?, snapshot: ScanStateSnapshot?): Answer {
        if (question == null || snapshot == null || !snapshot.hasScanData || snapshot.tracking == 3) return ABSTAINED
        return when (question) {
            StateQuestion.MISSING -> missing(snapshot)
            StateQuestion.PROGRESS -> progress(snapshot)
        }
    }

    private fun missing(s: ScanStateSnapshot): Answer {
        // (spoken text, cited region or null)
        val items = ArrayList<Pair<String, String?>>()
        if (!s.wallFound && s.walls.isEmpty()) items += "no wall found yet" to null
        for (w in s.walls.sortedBy { it.pct }) {
            if (w.pct < WALL_DONE_PCT) items += "${w.name}, ${w.pct} percent" to w.name
        }
        if (!s.floorFound) items += "the floor" to null
        // The ceiling is optional for completeness (real-room-fix): never listed as missing.
        for (g in s.gaps) items += g to g
        if (items.isEmpty()) {
            return if (s.completeness >= FINISH_COMPLETENESS) {
                Answer("Nothing is missing. You can finish.", abstained = false)
            } else {
                Answer("Nothing specific yet. Keep scanning.", abstained = false)
            }
        }
        val shown = items.take(MAX_LISTED)
        val more = items.size - shown.size
        val text = "Missing: " + shown.joinToString("; ") { it.first } + (if (more > 0) "; and $more more" else "") + "."
        return Answer(text, abstained = false, cites = shown.mapNotNull { it.second })
    }

    private fun progress(s: ScanStateSnapshot): Answer {
        val pct = Math.round(s.completeness * 100.0).toInt().coerceIn(0, 100)
        val n = s.wallCount
        val wallText = when (n) { 0 -> "no walls found yet"; 1 -> "1 wall found"; else -> "$n walls found" }
        return Answer("Scan is $pct percent complete, $wallText.", abstained = false)
    }
}
