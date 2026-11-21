package dev.capturekit.guidance

/**
 * Discrete spoken-guidance volume for "louder" / "quieter" (TextToSpeech KEY_PARAM_VOLUME, 0..1). Pure Kotlin.
 * Steps never reach silence: muting is the separate Voice toggle.
 */
class VolumeSteps(initialIndex: Int = DEFAULT_INDEX) {
    var index: Int = initialIndex.coerceIn(0, LEVELS.lastIndex)
        private set

    val volume: Float get() = LEVELS[index]

    /** Moves one step up; false = already at maximum. */
    fun louder(): Boolean { if (index >= LEVELS.lastIndex) return false; index++; return true }

    /** Moves one step down; false = already at minimum. */
    fun quieter(): Boolean { if (index <= 0) return false; index--; return true }

    companion object {
        val LEVELS = floatArrayOf(0.3f, 0.5f, 0.7f, 0.85f, 1.0f)
        const val DEFAULT_INDEX = 4   // full: today's behaviour is unchanged until the user asks for quieter
    }
}
