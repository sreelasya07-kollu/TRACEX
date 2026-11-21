package dev.capturekit.guidance

import dev.capturekit.capture.CoreEvent

/**
 * Recalibration prompt state (issue #1, ADR 0031). The core owns the decision (one C++ implementation for every
 * platform): it emits [CoreEvent.TrackingLost] when tracking has been degraded for 2 s and [CoreEvent.TrackingReset] when
 * it is back and steady. This only mirrors those events for the HUD. Immutable and pure, so it is JVM-testable; the
 * capture screen keeps the latest value in a volatile field (events arrive on the core event thread, the HUD reads it
 * on the main thread).
 */
data class Recalibration(
    /** The prompt is on screen: between TrackingLost and TrackingReset of the current session. */
    val active: Boolean = false,
    /** long_text of the latest RECALIBRATE instruction (the core's sentence); empty until one arrives. */
    val text: String = "",
    /** Completed recoveries in this session and the most recent one (drift delta for a debug line or log). */
    val resets: Int = 0,
    val lastReset: CoreEvent.TrackingReset? = null,
) {
    /** The next state after one core event. Events that do not concern recalibration return this same instance. */
    fun on(e: CoreEvent): Recalibration = when (e) {
        is CoreEvent.TrackingLost -> if (active) this else copy(active = true)
        is CoreEvent.TrackingReset -> copy(active = false, resets = resets + 1, lastReset = e)
        // Text only: visibility follows TrackingLost / TrackingReset, so a late instruction can never re-open the prompt.
        is CoreEvent.Instruction ->
            if (e.type == RECALIBRATE && e.longText.isNotEmpty() && e.longText != text) copy(text = e.longText) else this
        is CoreEvent.StateChange -> if (e.to in ENDED) Recalibration() else this
        else -> this
    }

    /** Body text for the overlay: the core's sentence, or a minimal fallback until the first RECALIBRATE arrives. */
    val prompt: String get() = text.ifEmpty { FALLBACK }

    companion object {
        /** Core instruction type name (guide::type_name). */
        const val RECALIBRATE = "RECALIBRATE"
        const val TITLE = "Recalibrating"
        const val FALLBACK = "Slowly turn your phone in a small circle."
        private val ENDED = setOf("FINALIZING", "COMPLETE", "FAILED")
    }
}
