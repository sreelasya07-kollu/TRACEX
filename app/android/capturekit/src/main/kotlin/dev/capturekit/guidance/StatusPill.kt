package dev.capturekit.guidance

/**
 * One plain-language status pill merging ARCore tracking, the core's frame-quality reasons and the thermal
 * policy level. Priority: tracking lost > too fast > hold steady > more light > warming up > good.
 * Pure and JVM-testable; labels are fixed status mappings (guidance sentences stay in the core).
 */
object StatusPill {
    enum class Tone(val argb: Int) {
        GOOD(0xFF2E7D32.toInt()), WARN(0xFFEF8F00.toInt()), BAD(0xFFC62828.toInt()), NEUTRAL(0xFF455A64.toInt())
    }
    data class Pill(val text: String, val tone: Tone)

    /** Every reason code the core quality module emits (core/quality/quality.cpp reason_code). */
    val REASON_CODES = listOf(
        "TRACKING_LOST", "TRACKING_LIMITED", "MOTION_BLUR", "BLUR", "SOFT_FOCUS", "OVEREXPOSED", "UNDEREXPOSED",
        "LOW_TEXTURE", "POOR_SPREAD", "NO_IMAGE", "NOT_NOVEL", "MIN_GAP", "NOT_BEST", "RECALIBRATING", "DUPLICATE",
    )

    val LOST = Pill("Tracking lost", Tone.BAD)
    /** Issue #1 / ADR 0031: the core is recalibrating tracking (keyframes paused). */
    val RECALIBRATING = Pill("Recalibrating", Tone.BAD)
    val TOO_FAST = Pill("Move slower", Tone.WARN)
    val HOLD = Pill("Hold steady", Tone.WARN)
    val LIGHT = Pill("More light", Tone.WARN)
    val HOT = Pill("Phone is hot", Tone.WARN)
    val WARMING = Pill("Warming up", Tone.WARN)
    val KEEP_GOING = Pill("Keep going", Tone.GOOD)
    val GOOD = Pill("Good", Tone.GOOD)
    val STARTING = Pill("Starting...", Tone.NEUTRAL)

    /** Lower = more urgent. */
    private fun rank(p: Pill): Int = when (p) {
        LOST -> 0; RECALIBRATING -> 1; TOO_FAST -> 2; HOLD -> 3; LIGHT -> 4; HOT -> 5; WARMING -> 6; KEEP_GOING -> 7; GOOD -> 8
        else -> 9
    }

    /**
     * Plain phrase for one core quality reason code; null = the reason never affects the pill. Unknown codes fall
     * back to "Keep going". Deviation from the brief's "others Keep going": NOT_NOVEL, MIN_GAP, NOT_BEST and DUPLICATE are
     * keyframe-selection reasons (normal while scanning), not image problems, so they are ignored.
     */
    fun forReason(code: String): Pill? = when (code) {
        "TRACKING_LOST" -> LOST
        "RECALIBRATING" -> RECALIBRATING
        "MOTION_BLUR" -> TOO_FAST
        "SOFT_FOCUS", "BLUR" -> HOLD
        "UNDEREXPOSED" -> LIGHT
        "NOT_NOVEL", "MIN_GAP", "NOT_BEST", "DUPLICATE" -> null
        else -> KEEP_GOING
    }

    /**
     * Plain phrase for an ARCore TrackingState name + TrackingFailureReason name (null = no tracking verdict).
     * "Starting..." only until tracking has first been reached ([everTracked]); after that a pause is "Hold steady".
     */
    fun forTracking(trackState: String, failure: String, everTracked: Boolean = false): Pill? = when (trackState) {
        "TRACKING" -> null
        "STOPPED" -> LOST
        "PAUSED" -> when (failure) {
            "EXCESSIVE_MOTION" -> TOO_FAST
            "INSUFFICIENT_LIGHT" -> LIGHT
            "NONE" -> if (everTracked) HOLD else STARTING
            else -> HOLD
        }
        else -> if (everTracked) HOLD else STARTING
    }

    /** Thermal policy level (0 NOMINAL .. 5 SAFE_PAUSE) to a pill, null when nominal. */
    fun forThermal(level: Int): Pill? = when {
        level >= 4 -> HOT
        level >= 1 -> WARMING
        else -> null
    }

    /**
     * The single pill: the most urgent of tracking, quality reasons and thermal. While tracking has not started
     * ("Starting...") nothing else is shown. No (relevant) reasons and nominal thermal = "Good".
     * [everTracked] = the camera has reached TRACKING at least once on this screen.
     */
    fun statusPill(trackState: String, failure: String, reasons: List<String>, thermalLevel: Int, everTracked: Boolean = false): Pill {
        val t = forTracking(trackState, failure, everTracked)
        if (t === STARTING) return STARTING
        var best = t ?: GOOD
        for (i in reasons.indices) { val p = forReason(reasons[i]) ?: continue; if (rank(p) < rank(best)) best = p }
        forThermal(thermalLevel)?.let { if (rank(it) < rank(best)) best = it }
        return best
    }
}
