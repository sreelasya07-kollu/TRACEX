package dev.capturekit.guidance

/**
 * Pure scheduling logic for spoken guidance (no Android types, time is passed in). Text always comes
 * from the core's templates; this only decides whether/when to say it. Not thread-safe: callers synchronize.
 * Priorities follow the core: 3 = urgent (blur, hold still, relocalize), 2 = guidance, 1 = info.
 */
class SpeechScheduler(private val cfg: Config = Config()) {
    /** Speech budget is a config value (phases/10), not a constant. */
    data class Config(
        val minGapMs: Long = 2_000,         // non-urgent: min time since the last utterance started/ended
        val maxPerMinute: Int = 6,          // non-urgent: utterances per rolling minute (real-room-fix T3: 8 -> 6, matches core max_prompts_per_min)
        val repeatWindowMs: Long = 12_000,  // same text suppressed inside this window
        val pendingTtlMs: Long = 4_000,     // a queued same-priority line is stale after this
        val urgentPriority: Int = 3,
        val speakTimeoutMs: Long = 10_000,  // no start/done/error callback for this long = engine is stuck; forget the utterance
    )

    sealed class Action {
        data class Speak(val utteranceId: String, val text: String, val priority: Int, val flush: Boolean) : Action()
        object Queued : Action()
        data class Drop(val reason: String) : Action()
    }

    private class Pending(val id: Int, val text: String, val priority: Int, val atMs: Long)

    @Volatile var enabled = true
    /** Called when a queued line is discarded unspoken: replaced by a newer line or past pendingTtlMs. */
    var onStale: (() -> Unit)? = null
    private val seenIds = HashSet<Int>()
    private val lastSaid = HashMap<String, Long>()
    private val startTimes = ArrayDeque<Long>()
    private var speakingId: String? = null
    private var speakingPriority = 0
    private var lastEndMs = Long.MIN_VALUE / 2
    private var lastStartMs = Long.MIN_VALUE / 2
    private var pending: Pending? = null
    private var manualSeq = 0

    /** Last text issued for speech (guidance or answer). Survives [reset] so "repeat" works after a pause. */
    var lastText: String? = null
        private set

    fun isSpeaking() = speakingId != null

    /**
     * Watchdog: if an utterance was issued/started more than speakTimeoutMs ago with no onDone, the engine never
     * called back (stuck). Clears the speaking state and returns true so the caller can stop the engine and drop focus.
     */
    fun expireStuck(nowMs: Long): Boolean {
        if (speakingId == null || nowMs - lastStartMs < cfg.speakTimeoutMs) return false
        speakingId = null; speakingPriority = 0; lastEndMs = nowMs
        return true
    }

    fun onInstruction(id: Int, text: String, priority: Int, nowMs: Long): Action {
        expireStuck(nowMs)
        if (!enabled) return Action.Drop("muted")
        if (text.isBlank()) return Action.Drop("empty")
        if (id in seenIds) return Action.Drop("repeat")
        val prev = lastSaid[text]
        if (prev != null && nowMs - prev < cfg.repeatWindowMs) { seenIds += id; return Action.Drop("repeat") }
        val urgent = priority >= cfg.urgentPriority
        if (speakingId != null) {
            return when {
                priority > speakingPriority -> speak(id, text, priority, nowMs, flush = true)
                priority == speakingPriority -> {
                    seenIds += id
                    if (pending != null) onStale?.invoke()
                    pending = Pending(id, text, priority, nowMs); Action.Queued
                }
                else -> Action.Drop("busy")
            }
        }
        if (!urgent) {
            if (nowMs - maxOf(lastEndMs, lastStartMs) < cfg.minGapMs) return Action.Drop("gap")
            while (startTimes.isNotEmpty() && nowMs - startTimes.first() >= 60_000) startTimes.removeFirst()
            if (startTimes.size >= cfg.maxPerMinute) return Action.Drop("budget")
        }
        return speak(id, text, priority, nowMs, flush = false)
    }

    /**
     * Voice-command speech (answer to "what is missing", volume feedback): user-requested, so it bypasses the
     * repeat window, the gap and the per-minute budget, and cuts any current utterance. Muted or blank = Drop.
     * [remember] false keeps [lastText] (so "repeat" does not repeat "Louder").
     */
    fun speakNow(text: String, nowMs: Long, remember: Boolean = true): Action {
        expireStuck(nowMs)
        if (!enabled) return Action.Drop("muted")
        if (text.isBlank()) return Action.Drop("empty")
        val id = -(++manualSeq)   // negative ids never collide with the core's instruction ids
        return speak(id, text, MANUAL_PRIORITY, nowMs, flush = speakingId != null, countBudget = false, remember = remember)
    }

    /** "Repeat that": the last spoken text again, bypassing the repeat window. Nothing said yet = Drop("nothing"). */
    fun repeatLast(nowMs: Long): Action {
        val t = lastText ?: return Action.Drop("nothing")
        return speakNow(t, nowMs)
    }

    private fun speak(
        id: Int, text: String, priority: Int, nowMs: Long, flush: Boolean,
        countBudget: Boolean = true, remember: Boolean = true,
    ): Action.Speak {
        seenIds += id
        lastSaid[text] = nowMs
        if (countBudget) startTimes.addLast(nowMs)
        if (remember) lastText = text
        speakingId = "ck-$id"; speakingPriority = priority; lastStartMs = nowMs
        if (flush && pending != null) { pending = null; onStale?.invoke() }
        return Action.Speak("ck-$id", text, priority, flush)
    }

    fun onStart(utteranceId: String, nowMs: Long) { if (utteranceId == speakingId) lastStartMs = nowMs }

    /** Utterance finished, errored or was stopped. */
    fun onDone(utteranceId: String, nowMs: Long) {
        if (utteranceId != speakingId) return   // a flushed utterance finishing late must not clear its successor
        speakingId = null; speakingPriority = 0; lastEndMs = nowMs
    }

    /** After onDone: the queued line to speak now, if it is still fresh. */
    fun nextPending(nowMs: Long): Action.Speak? {
        val p = pending ?: return null
        if (speakingId != null) return null
        pending = null
        if (!enabled) return null
        if (nowMs - p.atMs > cfg.pendingTtlMs) { onStale?.invoke(); return null }
        return speak(p.id, p.text, p.priority, nowMs, flush = false)
    }

    companion object { const val MANUAL_PRIORITY = 2 }

    fun reset() { seenIds.clear(); lastSaid.clear(); startTimes.clear(); speakingId = null; speakingPriority = 0; pending = null }
}
