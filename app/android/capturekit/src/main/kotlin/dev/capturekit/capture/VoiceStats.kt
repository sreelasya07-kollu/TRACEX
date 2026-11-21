package dev.capturekit.capture

import android.content.Context
import dev.capturekit.guidance.SpeechScheduler

/**
 * Per-session spoken-guidance counters (phases/10): prompts spoken (by priority), drops by reason
 * (incl. "stale" queued lines), user "repeat" requests, prompts/min and the median issue -> TTS
 * onStart latency. Rates divide by at least one minute. Pure JVM (time is passed in);
 * synchronized because TTS callbacks arrive on a binder thread. Feeds the CK/Voice log and the
 * research log; never part of the bundle.
 */
class VoiceStats(private val maxLatencySamples: Int = 1_000) {
    data class Snapshot(
        val prompts: Int,
        val promptsByPriority: Map<Int, Int>,
        val queued: Int,
        val drops: Map<String, Int>,
        val repeatRequests: Int,
        val durationMs: Long,
        val promptsPerMin: Double,
        val ttsStartMedianMs: Long?,
    ) {
        /** Repeats the scheduler suppressed (same id or same text inside the repeat window). */
        val repeatsSuppressed: Int get() = drops["repeat"] ?: 0

        /** Non-urgent prompts (priority < urgentPriority) per minute: the rate SpeechScheduler budgets. */
        fun nonUrgentPerMin(urgentPriority: Int): Double =
            promptsByPriority.filterKeys { it < urgentPriority }.values.sum() * 60_000.0 / rateDenominatorMs(durationMs)

        fun withinBudget(cfg: SpeechScheduler.Config): Boolean = nonUrgentPerMin(cfg.urgentPriority) <= cfg.maxPerMinute

        fun logLine(): String {
            val d = drops.entries.sortedBy { it.key }.joinToString(",") { "${it.key}=${it.value}" }.ifEmpty { "-" }
            return "prompts $prompts queued $queued drops $d repeatReq $repeatRequests " +
                "perMin ${fmt(promptsPerMin)} ttsStartMedMs ${ttsStartMedianMs ?: "-"}"
        }

        /** Flat JSON object (hand-written: org.json is stubbed in JVM unit tests). */
        fun toJson(): String {
            val d = drops.entries.sortedBy { it.key }.joinToString(",") { "\"${it.key}\":${it.value}" }
            val bp = promptsByPriority.entries.sortedBy { it.key }.joinToString(",") { "\"${it.key}\":${it.value}" }
            return "{\"prompts\":$prompts,\"prompts_by_priority\":{$bp},\"queued\":$queued,\"drops\":{$d},\"repeats_suppressed\":$repeatsSuppressed," +
                "\"repeat_requests\":$repeatRequests,\"duration_ms\":$durationMs,\"prompts_per_min\":${fmt(promptsPerMin)}," +
                "\"tts_start_median_ms\":${ttsStartMedianMs ?: "null"}}"
        }
    }

    private var startMs: Long? = null
    private val byPriority = HashMap<Int, Int>()
    private var queued = 0
    private var repeatRequests = 0
    private val drops = HashMap<String, Int>()
    private val issuedAt = LinkedHashMap<String, Long>()   // insertion order: eldest evicted at the cap
    private val latencies = ArrayList<Long>()

    /** Session start (first call wins until reset). */
    @Synchronized fun begin(nowMs: Long) { if (startMs == null) startMs = nowMs }

    /** Record what SpeechScheduler decided (onInstruction or nextPending). */
    @Synchronized fun onAction(action: SpeechScheduler.Action?, nowMs: Long) {
        begin(nowMs)
        when (action) {
            is SpeechScheduler.Action.Speak -> {
                byPriority.merge(action.priority, 1, Int::plus)
                issuedAt.remove(action.utteranceId)
                if (issuedAt.size >= maxLatencySamples) issuedAt.remove(issuedAt.keys.first())
                issuedAt[action.utteranceId] = nowMs
            }
            is SpeechScheduler.Action.Queued -> queued++
            is SpeechScheduler.Action.Drop -> drops.merge(action.reason, 1, Int::plus)
            null -> Unit
        }
    }

    /** TTS onStart: issue -> audible latency. Unknown or already-started ids are ignored. */
    @Synchronized fun onTtsStart(utteranceId: String, nowMs: Long) {
        val t0 = issuedAt.remove(utteranceId) ?: return
        if (latencies.size < maxLatencySamples) latencies += (nowMs - t0).coerceAtLeast(0)
    }

    /** Utterance finished, errored or was stopped (TTS onDone/onError/onStop): forget its issue time. */
    @Synchronized fun onUtteranceEnd(utteranceId: String) { issuedAt.remove(utteranceId) }

    /** A queued line was discarded unspoken (replaced or expired): wire to SpeechScheduler.onStale. */
    @Synchronized fun onStale() { drops.merge(STALE, 1, Int::plus) }

    /** User asked to hear the last line again (voice "repeat" or a button). */
    @Synchronized fun onRepeatRequest() { repeatRequests++ }

    @Synchronized fun snapshot(nowMs: Long): Snapshot {
        val dur = startMs?.let { (nowMs - it).coerceAtLeast(0) } ?: 0L
        val prompts = byPriority.values.sum()
        val perMin = prompts * 60_000.0 / rateDenominatorMs(dur)
        return Snapshot(prompts, HashMap(byPriority), queued, HashMap(drops), repeatRequests, dur, perMin, median(latencies))
    }

    @Synchronized fun reset() {
        startMs = null; byPriority.clear(); queued = 0; repeatRequests = 0
        drops.clear(); issuedAt.clear(); latencies.clear()
    }

    companion object {
        const val STALE = "stale"

        /** Rates use at least one minute so a single early prompt is not extrapolated to a burst. */
        fun rateDenominatorMs(durationMs: Long): Double = maxOf(durationMs, 60_000L).toDouble()

        fun median(xs: List<Long>): Long? {
            if (xs.isEmpty()) return null
            val s = xs.sorted()
            val m = s.size / 2
            return if (s.size % 2 == 1) s[m] else (s[m - 1] + s[m]) / 2
        }

        internal fun fmt(x: Double): String = String.format(java.util.Locale.ROOT, "%.2f", x)
    }
}

/** Minimal key-value seam so the budget codec is JVM-testable without Robolectric. */
interface LongStore {
    fun getLong(key: String): Long?
    fun putLongs(values: Map<String, Long>)
}

/**
 * Speech budget (SpeechScheduler.Config) persisted in SharedPreferences. Missing or invalid values
 * (non-positive, out of Int range) fall back to the defaults, field by field.
 */
object SpeechBudgetPrefs {
    const val PREFS_NAME = "capturekit_speech_budget"
    const val MIN_GAP_MS = "min_gap_ms"
    const val MAX_PER_MINUTE = "max_per_minute"
    const val REPEAT_WINDOW_MS = "repeat_window_ms"
    const val PENDING_TTL_MS = "pending_ttl_ms"
    const val URGENT_PRIORITY = "urgent_priority"
    const val SPEAK_TIMEOUT_MS = "speak_timeout_ms"

    fun load(store: LongStore, defaults: SpeechScheduler.Config = SpeechScheduler.Config()): SpeechScheduler.Config {
        fun long(k: String, d: Long, min: Long) = store.getLong(k)?.takeIf { it >= min } ?: d
        fun int(k: String, d: Int, min: Int) = store.getLong(k)?.takeIf { it >= min && it <= Int.MAX_VALUE }?.toInt() ?: d
        return SpeechScheduler.Config(
            minGapMs = long(MIN_GAP_MS, defaults.minGapMs, 0),
            maxPerMinute = int(MAX_PER_MINUTE, defaults.maxPerMinute, 1),
            repeatWindowMs = long(REPEAT_WINDOW_MS, defaults.repeatWindowMs, 0),
            pendingTtlMs = long(PENDING_TTL_MS, defaults.pendingTtlMs, 0),
            urgentPriority = int(URGENT_PRIORITY, defaults.urgentPriority, 1),
            speakTimeoutMs = long(SPEAK_TIMEOUT_MS, defaults.speakTimeoutMs, 1),
        )
    }

    fun save(store: LongStore, cfg: SpeechScheduler.Config) = store.putLongs(
        mapOf(
            MIN_GAP_MS to cfg.minGapMs,
            MAX_PER_MINUTE to cfg.maxPerMinute.toLong(),
            REPEAT_WINDOW_MS to cfg.repeatWindowMs,
            PENDING_TTL_MS to cfg.pendingTtlMs,
            URGENT_PRIORITY to cfg.urgentPriority.toLong(),
            SPEAK_TIMEOUT_MS to cfg.speakTimeoutMs,
        ),
    )

    /** SharedPreferences-backed store (app-private). */
    fun store(context: Context): LongStore {
        val p = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return object : LongStore {
            override fun getLong(key: String): Long? = if (p.contains(key)) p.getLong(key, 0L) else null
            override fun putLongs(values: Map<String, Long>) {
                val e = p.edit()
                values.forEach { (k, v) -> e.putLong(k, v) }
                e.apply()
            }
        }
    }

    fun load(context: Context): SpeechScheduler.Config = load(store(context))
    fun save(context: Context, cfg: SpeechScheduler.Config) = save(store(context), cfg)
}
