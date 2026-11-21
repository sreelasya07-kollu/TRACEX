package dev.capturekit.guidance

/** Maps the core's Instruction.haptic name ("none" | "tap" | "double" | "success") to a pulse plan. Pure, JVM-testable. */
object HapticMap {
    enum class Kind { TICK, CONFIRM }
    data class Pulse(val kind: Kind, val delayMs: Long)

    private const val DOUBLE_GAP_MS = 140L

    fun pulses(haptic: String, priority: Int = 2): List<Pulse> = when (haptic) {
        "none" -> emptyList()
        "tap" -> listOf(Pulse(Kind.TICK, 0))
        "double" -> listOf(Pulse(Kind.TICK, 0), Pulse(Kind.TICK, DOUBLE_GAP_MS))
        "success" -> listOf(Pulse(Kind.CONFIRM, 0))
        else -> if (priority >= 3) listOf(Pulse(Kind.TICK, 0), Pulse(Kind.TICK, DOUBLE_GAP_MS)) else emptyList()
    }
}
