package dev.capturekit.capture

/**
 * Feature switches (issue #6, ADR 0032; generalises the U11-A debug overrides). The default build shows only features
 * that passed on the phone (docs/DEVICE_TEST_LOG.md); everything else is OFF here and can be switched on in a
 * debuggable build only: `adb shell setprop debug.capturekit.<name> 1` (0 forces off), then force-stop the app
 * (the host reads the properties once per process). An unset, blank or junk value keeps the default.
 *  - `guidance`: navigation / progress prompts, live room card, weak-wall review sheet, review room drawing
 *  - `detector`: 2D detector, camera boxes chip, object labels, review "Found" counts (supersedes ADR 0030 "on")
 *  - `voice_commands`: push-to-talk mic chip, VoiceController, StateAnswers
 *  - `recalibrate_card`: the issue #1 recalibration card (the core keyframe pause always stays)
 *  - `segmentation`: issue #8 camera mask overlay + Masks chip (also needs the packaged SAM 2.1 model files)
 *  - `understanding 0|1|2`: core `guide.understanding` override (unchanged from U11-A)
 *  - `record_imu`: research only, keep the IMU channels in the bundle (ADR 0034; the default bundle is lean)
 * The core still records every instruction in /events; only display, speech and haptics are gated.
 */
object FeatureFlags {
    const val PROP_UNDERSTANDING = "debug.capturekit.understanding"
    const val PROP_DETECTOR = "debug.capturekit.detector"
    const val PROP_GUIDANCE = "debug.capturekit.guidance"
    const val PROP_VOICE_COMMANDS = "debug.capturekit.voice_commands"
    const val PROP_RECALIBRATE_CARD = "debug.capturekit.recalibrate_card"
    const val PROP_SEGMENTATION = "debug.capturekit.segmentation"
    const val PROP_RECORD_IMU = "debug.capturekit.record_imu"

    /** Understanding level when the detector runs and no property overrides it: objects only, no gap steering. */
    const val DEFAULT_UNDERSTANDING_WITH_DETECTOR = 1

    /**
     * Instruction types shown/spoken/felt with guidance OFF: the safety and tracking prompts (core InstrType SlowDown,
     * HoldStill, Relocalize, MoreLight, AimTextured, Recalibrate). Every other type (navigation, Progress, Finish) is guidance.
     */
    val SAFETY_INSTRUCTIONS: Set<String> = setOf("SLOW_DOWN", "HOLD_STILL", "RELOCALIZE", "MORE_LIGHT", "AIM_TEXTURED", "RECALIBRATE")

    /** Resolved switches for one process. [DEFAULTS] = release build or no property set. */
    data class Flags(
        val understanding: Int? = null,
        val detector: Boolean = false,
        val guidance: Boolean = false,
        val voiceCommands: Boolean = false,
        val recalibrateCard: Boolean = false,
        val segmentation: Boolean = false,
        /** Research only (ADR 0034): keep /imu/gyro and /imu/accel in the bundle. Off = the lean bundle. */
        val recordImu: Boolean = false,
    ) {
        companion object { val DEFAULTS = Flags() }
    }

    /** "1" = on, "0" = off (surrounding whitespace ignored); anything else (unset, blank, junk) keeps [default]. */
    fun switchOn(raw: String?, default: Boolean): Boolean = when (raw?.trim()) {
        "1" -> true
        "0" -> false
        else -> default
    }

    /** Flags from property values ([get] returns the raw value or null). Call only in a debuggable build. */
    fun fromProps(get: (String) -> String?): Flags {
        val d = Flags.DEFAULTS
        return Flags(
            understanding = understandingLevel(get(PROP_UNDERSTANDING)),
            detector = detectorOn(get(PROP_DETECTOR)),
            guidance = switchOn(get(PROP_GUIDANCE), d.guidance),
            voiceCommands = switchOn(get(PROP_VOICE_COMMANDS), d.voiceCommands),
            recalibrateCard = switchOn(get(PROP_RECALIBRATE_CARD), d.recalibrateCard),
            segmentation = switchOn(get(PROP_SEGMENTATION), d.segmentation),
            recordImu = switchOn(get(PROP_RECORD_IMU), d.recordImu),
        )
    }

    /** Property prefix of every switch; [parseGetprop] keeps only these. */
    const val PROP_PREFIX = "debug.capturekit."

    /**
     * Output of one argument-less `getprop` call ("[name]: [value]" per line) -> the `debug.capturekit.*` entries.
     * Malformed lines are skipped; a repeated name keeps the last value.
     */
    fun parseGetprop(output: String): Map<String, String> {
        val m = HashMap<String, String>()
        for (line in output.lineSequence()) {
            val r = GETPROP_LINE.matchEntire(line.trim()) ?: continue
            val name = r.groupValues[1]
            if (name.startsWith(PROP_PREFIX)) m[name] = r.groupValues[2]
        }
        return m
    }
    private val GETPROP_LINE = Regex("""\[([^\]]+)\]:\s*\[(.*)\]""")

    /**
     * Coaching-line rule for the latest shown instruction (ADR 0032). With [guidance] on it stays until the next one
     * (unchanged behaviour). With guidance off only safety prompts arrive and nothing would replace them, so a prompt
     * is shown for [holdMs] after it arrived, then the screen falls back to the plain scan prompt; a RECALIBRATE prompt
     * stays while the core is still recovering ([recalibrating]).
     */
    fun instructionStillShown(text: String, arrivedMs: Long, nowMs: Long, guidance: Boolean, recalibrating: Boolean,
                              holdMs: Long = PROMPT_HOLD_MS): Boolean =
        text.isNotEmpty() && (guidance || recalibrating || nowMs - arrivedMs < holdMs)

    /** How long a safety prompt stays on the coaching line with guidance off (guess; tune on device). */
    const val PROMPT_HOLD_MS = 4_000L

    /** 0 = off, 1 = objects, 2 = objects + gap steering (core `guide.understanding`); null = property unset / blank / junk / out of range. */
    fun understandingLevel(raw: String?): Int? = raw?.trim()?.toIntOrNull()?.takeIf { it in 0..2 }

    /** The detector is off unless the property is exactly "1" (ADR 0032; it still needs the packaged model). */
    fun detectorOn(raw: String?): Boolean = switchOn(raw, Flags.DEFAULTS.detector)

    /** Show / speak / vibrate an instruction of core [type]: always with [guidance], else only the [SAFETY_INSTRUCTIONS]. */
    fun showInstruction(type: String, guidance: Boolean): Boolean = guidance || type in SAFETY_INSTRUCTIONS

    /**
     * Understanding level to pass: the [override] when set (including 0), else [DEFAULT_UNDERSTANDING_WITH_DETECTOR]
     * when [detectorWillRun], else null (no key: the core default, off). Without a detector, level 1 would only add
     * geometry-only outputs, so the session is left exactly as before.
     */
    fun understandingFor(override: Int?, detectorWillRun: Boolean): Int? =
        override ?: if (detectorWillRun) DEFAULT_UNDERSTANDING_WITH_DETECTOR else null

    /**
     * Extra session config for [SessionManager.start]: `guide.understanding` is passed explicitly whenever a level is
     * given (including 0), null = no key; `record_imu` = 1 only when [recordImu] (the core default is lean: no IMU).
     */
    fun extraConfig(understanding: Int?, recordImu: Boolean = false): Map<String, Any> =
        buildMap {
            if (understanding != null && understanding in 0..2) put("guide.understanding", understanding)
            if (recordImu) put("record_imu", 1)
        }
}
