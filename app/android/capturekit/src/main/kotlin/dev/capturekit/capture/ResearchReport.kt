package dev.capturekit.capture

import java.io.File

/** Trial arm: unguided mutes voice and coaching on the device; the core still records everything. */
enum class TrialArm(val wire: String) {
    GUIDED("guided"), UNGUIDED("unguided");

    companion object { fun fromWire(s: String?): TrialArm = values().firstOrNull { it.wire == s } ?: GUIDED }
}

/** Who/where for one trial scan. [participant] is a pseudonymous code, never a name. */
data class TrialSetup(val participant: String, val arm: TrialArm, val room: String = "") {
    companion object {
        private val CODE = Regex("[A-Za-z0-9_-]{1,32}")

        /** Trimmed code when it is 1..32 of [A-Za-z0-9_-], else null. */
        fun cleanCode(raw: String?): String? = raw?.trim()?.takeIf { CODE.matches(it) }

        /** Room label: trimmed, control characters removed, at most 48 chars (may be empty). */
        fun cleanRoom(raw: String?): String = raw.orEmpty().filter { !it.isISOControl() }.trim().take(48)
    }
}

/** Governor/thermal level changes during a scan (pure, thread-safe). Times are session-relative ms. */
class ThermalTrace {
    private var max = 0
    private var last = -1
    private val changes = ArrayList<Pair<Long, Int>>()

    @Synchronized fun onLevel(level: Int, tMs: Long) {
        if (level == last) return
        last = level
        if (level > max) max = level
        if (changes.size < MAX_CHANGES) changes += tMs to level
    }

    @Synchronized fun maxLevel() = max
    @Synchronized fun reset() { max = 0; last = -1; changes.clear() }
    @Synchronized fun changesSnapshot(): List<Pair<Long, Int>> = ArrayList(changes)

    private companion object { const val MAX_CHANGES = 200 }
}

/** Counts of voice intents used (name -> count), thread-safe. */
class IntentCounter {
    private val m = java.util.TreeMap<String, Int>()
    @Synchronized fun add(name: String) { m.merge(name, 1, Int::plus) }
    @Synchronized fun snapshot(): Map<String, Int> = LinkedHashMap(m)
    @Synchronized fun reset() { m.clear() }
}

/**
 * The field research record: `<bundle name>.research.json` written NEXT TO the bundle directory, never inside it
 * (bundle schema untouched) and never part of an upload (uploads send the bundle directory only). Pure JVM; JSON is
 * hand-written because org.json is stubbed in JVM unit tests. Keys match what `ckit e2e` reads.
 */
object ResearchReport {
    const val SCHEMA = "research/1"

    data class Input(
        val bundleName: String,
        val trial: TrialSetup,
        val device: String,
        val appVersion: String,
        val durationMs: Long,
        val keyframes: Int,
        val finishStatus: Int,
        val ingest: Map<String, Long>,
        val thermalMaxLevel: Int,
        val thermalChanges: List<Pair<Long, Int>>,
        val instructionsEmitted: Int,
        val voice: VoiceStats.Snapshot,
        val intents: Map<String, Int>,
    )

    /** Sibling file of a bundle directory (same parent, never a child). */
    fun fileFor(bundleDir: File): File = File(bundleDir.absoluteFile.parentFile, bundleDir.name + ".research.json")

    /** True for the research file (or its temp file) of a bundle: not a bundle, skip it when listing a bundles directory. */
    fun isResearchFile(name: String) = name.endsWith(".research.json") || name.endsWith(".research.json.tmp")

    /** Deletes the research file and its temp file beside [bundleDir]; call wherever the bundle directory is deleted. */
    fun deleteSiblings(bundleDir: File) {
        val f = fileFor(bundleDir)
        f.delete()
        File(f.parentFile, f.name + ".tmp").delete()
    }

    /** Counters worth keeping from [IngestStats] (public atomics only). */
    fun ingestMap(s: IngestStats): Map<String, Long> = linkedMapOf(
        "ar_frames" to s.arFrames.get(),
        "tracking_poses" to s.trackingPoses.get(),
        "other_poses" to s.otherPoses.get(),
        "frame_calls" to s.frameCalls.get(),
        "frames_ingested" to s.framesIngested.get(),
        "luma_short" to s.lumaShort.get(),
        "on_frame_throws" to s.onFrameThrows.get(),
        "points_ingested" to s.pointsIngested.get(),
        "planes_ingested" to s.planesIngested.get(),
        "keyframe_requests" to s.keyframes.get(),
        "ar_dup_frames" to s.arDupFrames.get(),
        "gl_skipped_dup" to s.glSkippedDup.get(),
        "ring_busy" to s.ringBusy.get(),
        "detector_runs" to s.detectorRuns.get(),
        "detector_dropped" to s.detectorDropped.get(),
        "candidate_stride" to s.candidateStride.toLong(),
    )

    fun toJson(i: Input): String {
        val ing = i.ingest.entries.joinToString(",") { "${q(it.key)}:${it.value}" }
        val thermal = i.thermalChanges.joinToString(",") { "{\"t_ms\":${it.first},\"level\":${it.second}}" }
        val level = Policy.LEVELS.getOrElse(i.thermalMaxLevel) { "?" }
        val intents = i.intents.entries.sortedBy { it.key }.joinToString(",") { "${q(it.key)}:${it.value}" }
        return "{\"schema\":${q(SCHEMA)},\"bundle\":${q(i.bundleName)},\"participant\":${q(i.trial.participant)}," +
            "\"arm\":${q(i.trial.arm.wire)},\"room\":${q(i.trial.room)},\"device\":${q(i.device)},\"app_version\":${q(i.appVersion)}," +
            "\"duration_ms\":${i.durationMs},\"keyframes\":${i.keyframes},\"finish_status\":${i.finishStatus}," +
            "\"ingest\":{$ing},\"thermal\":{\"max_level\":${i.thermalMaxLevel},\"max_level_name\":${q(level)},\"changes\":[$thermal]}," +
            "\"instructions_emitted\":${i.instructionsEmitted},\"voice_stats\":${i.voice.toJson()}," +
            "\"tts_start_median_ms\":${i.voice.ttsStartMedianMs ?: "null"},\"intents\":{$intents}}"
    }

    /** Writes via a temp file and rename so a kill never leaves a half file. False on I/O failure. */
    fun write(bundleDir: File, json: String): Boolean = try {
        val f = fileFor(bundleDir)
        val tmp = File(f.parentFile, f.name + ".tmp")
        tmp.writeText(json, Charsets.UTF_8)
        if (f.exists()) f.delete()
        tmp.renameTo(f)
    } catch (_: java.io.IOException) { false }

    internal fun q(s: String): String {
        val b = StringBuilder("\"")
        for (c in s) when {
            c == '"' -> b.append("\\\"")
            c == '\\' -> b.append("\\\\")
            c == '\n' -> b.append("\\n")
            c == '\r' -> b.append("\\r")
            c == '\t' -> b.append("\\t")
            c < ' ' -> b.append(String.format(java.util.Locale.ROOT, "\\u%04x", c.code))
            else -> b.append(c)
        }
        return b.append('"').toString()
    }
}
