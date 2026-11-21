package dev.capturekit.guidance

/**
 * Spoken-command parser (Phase 10). Pure Kotlin, no Android types: the shell hands in the
 * recogniser's N-best list (RESULTS_RECOGNITION + CONFIDENCE_SCORES) and gets back one intent
 * or NoMatch. Vocabulary is closed and lives in [PhraseTable] data per locale; no LLM, no network.
 */
enum class VoiceIntent(val requiresConfirmation: Boolean = false) {
    PAUSE, RESUME, REPEAT, MISSING,
    /** Ends the scan: the shell must get a spoken or tap confirmation before acting. */
    FINISH(requiresConfirmation = true),
    LOUDER, QUIETER,
}

/** One recogniser hypothesis. [confidence] in 0..1; null or <= 0 means "not reported". */
data class Hypothesis(val text: String, val confidence: Float? = null)

sealed interface VoiceParse {
    data class Match(
        val intent: VoiceIntent,
        val score: Float,
        val heard: String,
    ) : VoiceParse {
        val needsConfirmation: Boolean get() = intent.requiresConfirmation
    }

    data class NoMatch(val reason: String) : VoiceParse
}

enum class Confirmation { YES, NO, UNKNOWN }

/**
 * Locale vocabulary as data. Phrases are written naturally; they are normalised with the same
 * rules as the utterance, so punctuation/case in the table do not matter. Add a locale by adding
 * another [PhraseTable] to [PhraseTables.all]; the parser has no language-specific code.
 */
data class PhraseTable(
    val locale: String,
    val phrases: Map<VoiceIntent, List<String>>,
    /** Words dropped before matching ("um", "please"). Multi-word fillers allowed. */
    val fillers: List<String>,
    /** Any of these tokens anywhere in the utterance -> NoMatch (never act on "don't stop"). */
    val negations: List<String>,
    val yes: List<String>,
    val no: List<String>,
)

object PhraseTables {
    val EN = PhraseTable(
        locale = "en",
        phrases = mapOf(
            VoiceIntent.PAUSE to listOf(
                "pause", "stop", "hold on", "hold", "wait", "pause scan", "pause the scan",
                "pause scanning", "stop scan", "stop the scan", "stop scanning", "take a break",
            ),
            VoiceIntent.RESUME to listOf(
                "resume", "continue", "go on", "carry on", "keep going", "resume scan",
                "resume the scan", "resume scanning", "continue scan", "continue scanning",
                "unpause", "go again",
            ),
            VoiceIntent.REPEAT to listOf(
                "repeat", "repeat that", "say again", "say that again", "say it again",
                "what did you say", "come again", "pardon",
            ),
            VoiceIntent.MISSING to listOf(
                "what is missing", "what's missing", "what is left", "what's left",
                "how much is left", "how much left", "how much more", "what remains",
                "what else", "what is left to scan", "what's left to scan", "where next", "what next",
                "what's next", "missing",
            ),
            VoiceIntent.FINISH to listOf(
                "finish", "finish scan", "finish the scan", "finish scanning", "i'm done",
                "i am done", "done", "end scan", "end the scan", "we're done", "all done",
                "complete scan", "that's it",
            ),
            VoiceIntent.LOUDER to listOf(
                "louder", "speak up", "volume up", "turn it up", "turn up the volume",
                "can't hear you", "cannot hear you", "more volume",
            ),
            VoiceIntent.QUIETER to listOf(
                "quieter", "softer", "volume down", "turn it down", "turn down the volume",
                "too loud", "less volume", "lower the volume", "lower volume",
            ),
        ),
        fillers = listOf(
            "um", "umm", "uh", "uhh", "er", "erm", "ah", "hmm", "please", "okay", "ok", "hey",
            "so", "now", "just", "like", "well", "the app", "app", "capturekit", "could you",
            "can you", "would you", "will you", "kindly", "thanks", "thank you",
        ),
        negations = listOf("don't", "dont", "do not", "not", "never", "no"),
        yes = listOf(
            "yes", "yeah", "yep", "yup", "sure", "confirm", "correct", "finish", "yes finish",
            "do it", "go ahead", "affirmative",
        ),
        no = listOf("no", "nope", "cancel", "don't", "do not", "not yet", "keep scanning", "continue", "wait"),
    )

    val all: Map<String, PhraseTable> = listOf(EN).associateBy { it.locale }

    /** "en-US" -> en table. Unknown language -> null (shell hides the mic and says so). */
    fun forLocale(tag: String): PhraseTable? {
        val lang = tag.lowercase().replace('_', '-').substringBefore('-')
        return all[tag.lowercase()] ?: all[lang]
    }
}

class VoiceIntentParser(
    private val table: PhraseTable,
    private val config: Config = Config(),
) {
    data class Config(
        /** Minimum combined score (match quality x recogniser confidence) to act. */
        val minScore: Float = 0.45f,
        /** Quality of a phrase found inside a longer utterance vs. an exact match (1.0). */
        val containedQuality: Float = 0.8f,
        /** Extra tokens tolerated around a contained phrase; more = background talk -> NoMatch. */
        val maxExtraTokens: Int = 3,
        /** Prior when the recogniser reports no confidence: rankDecay^rank. */
        val rankDecay: Float = 0.8f,
        /** Two different intents within this score margin -> ambiguous -> NoMatch. */
        val ambiguityMargin: Float = 0.1f,
    )

    private class Entry(val intent: VoiceIntent, val tokens: List<String>)

    private val fillers: List<List<String>> =
        table.fillers.map { tokens(it) }.filter { it.isNotEmpty() }.sortedByDescending { it.size }
    private val negations: List<List<String>> = table.negations.map { tokens(it) }.filter { it.isNotEmpty() }
    private val entries: List<Entry> = table.phrases.flatMap { (intent, list) ->
        list.map { Entry(intent, stripFillers(tokens(it))) }.filter { it.tokens.isNotEmpty() }
    }
    private val yes = table.yes.map { stripFillers(tokens(it)) }.filter { it.isNotEmpty() }
    private val no = table.no.map { stripFillers(tokens(it)) }.filter { it.isNotEmpty() }

    init {
        // A phrase listed under two intents makes the table ambiguous: fail at construction.
        val dup = entries.groupBy { it.tokens }.filterValues { e -> e.map { it.intent }.toSet().size > 1 }
        require(dup.isEmpty()) { "phrase table ${table.locale}: phrase under several intents: ${dup.keys}" }
    }

    fun parse(text: String): VoiceParse = parse(listOf(Hypothesis(text)))

    fun parse(nBest: List<Hypothesis>): VoiceParse {
        if (nBest.isEmpty()) return VoiceParse.NoMatch("empty")
        var best: VoiceParse.Match? = null
        var bestOther: VoiceParse.Match? = null // best match of a different intent than `best`
        var lastReason = "no_phrase"
        val priors = priors(nBest)
        nBest.forEachIndexed { rank, h ->
            val prior = priors[rank]
            when (val m = matchOne(h.text)) {
                is MatchResult.Hit -> {
                    val c = VoiceParse.Match(m.intent, m.quality * prior, h.text)
                    val b = best
                    if (b == null || c.score > b.score) {
                        if (b != null && b.intent != c.intent) bestOther = b
                        best = c
                    } else if (c.intent != b.intent && (bestOther == null || c.score > bestOther!!.score)) {
                        bestOther = c
                    }
                }
                is MatchResult.Miss -> if (rank == 0) lastReason = m.reason
            }
        }
        val b = best ?: return VoiceParse.NoMatch(lastReason)
        val o = bestOther
        if (o != null && b.score - o.score < config.ambiguityMargin) return VoiceParse.NoMatch("ambiguous")
        if (b.score < config.minScore) return VoiceParse.NoMatch("low_confidence")
        return b
    }

    /** Spoken answer to "Finish the scan?". Anything unclear is UNKNOWN (shell keeps the tap buttons). */
    /**
     * Same priors and [Config.minScore] as [parse]. A yes and a no anywhere in the N-best = UNKNOWN,
     * so a finish is never confirmed on a coin flip.
     */
    fun parseConfirmation(nBest: List<Hypothesis>): Confirmation {
        val priors = priors(nBest)
        var yesScore = -1f
        var noScore = -1f
        nBest.forEachIndexed { rank, h ->
            val t = stripFillers(tokens(h.text))
            if (t.isEmpty()) return@forEachIndexed
            val isNo = no.any { it == t }
            val isYes = yes.any { it == t }
            if (isNo && !isYes) noScore = maxOf(noScore, priors[rank])
            if (isYes && !isNo) yesScore = maxOf(yesScore, priors[rank])
        }
        if (yesScore >= 0f && noScore >= 0f) return Confirmation.UNKNOWN
        return when {
            yesScore >= config.minScore -> Confirmation.YES
            noScore >= config.minScore -> Confirmation.NO
            else -> Confirmation.UNKNOWN
        }
    }

    /**
     * Recogniser prior per rank. A reported confidence (> 0) is used as is. Unreported (null or 0)
     * gets rankDecay^rank; if any hypothesis reports a confidence, that is also capped at
     * rankDecay x the prior of the hypothesis ranked above, so an unreported alternative never
     * ties or outranks a reported one above it.
     */
    private fun priors(nBest: List<Hypothesis>): FloatArray {
        val anyReported = nBest.any { (it.confidence ?: 0f) > 0f }
        val out = FloatArray(nBest.size)
        nBest.forEachIndexed { rank, h ->
            val reported = h.confidence?.takeIf { it > 0f }?.coerceAtMost(1f)
            out[rank] = when {
                reported != null -> reported
                anyReported && rank > 0 -> minOf(pow(config.rankDecay, rank), out[rank - 1] * config.rankDecay)
                else -> pow(config.rankDecay, rank)
            }
        }
        return out
    }

    fun parseConfirmation(text: String): Confirmation = parseConfirmation(listOf(Hypothesis(text)))

    private sealed interface MatchResult {
        data class Hit(val intent: VoiceIntent, val quality: Float) : MatchResult
        data class Miss(val reason: String) : MatchResult
    }

    private fun matchOne(text: String): MatchResult {
        val raw = tokens(text)
        if (raw.isEmpty()) return MatchResult.Miss("empty")
        if (negations.any { indexOf(raw, it) >= 0 }) return MatchResult.Miss("negated")
        val t = stripFillers(raw)
        if (t.isEmpty()) return MatchResult.Miss("filler_only")
        entries.firstOrNull { it.tokens == t }?.let { return MatchResult.Hit(it.intent, 1f) }
        // Contained phrases: the longest one wins; equal-length phrases of different intents = ambiguous.
        val hits = entries.filter { t.size - it.tokens.size <= config.maxExtraTokens && indexOf(t, it.tokens) >= 0 }
        if (hits.isEmpty()) return MatchResult.Miss("no_phrase")
        val longest = hits.maxOf { it.tokens.size }
        val top = hits.filter { it.tokens.size == longest }.map { it.intent }.toSet()
        // A shorter phrase of another intent inside the longest one is fine ("stop scanning" vs "stop"),
        // but a shorter one elsewhere in the utterance means two commands were spoken.
        val other = hits.filter { it.intent !in top }.any { s ->
            val inLongest = hits.filter { it.tokens.size == longest }.any { indexOf(it.tokens, s.tokens) >= 0 }
            !inLongest
        }
        if (top.size > 1 || other) return MatchResult.Miss("ambiguous")
        return MatchResult.Hit(top.single(), config.containedQuality)
    }

    private fun stripFillers(t: List<String>): List<String> {
        if (fillers.isEmpty()) return t
        val out = ArrayList<String>(t.size)
        var i = 0
        outer@ while (i < t.size) {
            for (f in fillers) {
                if (i + f.size <= t.size && (f.indices).all { t[i + it] == f[it] }) { i += f.size; continue@outer }
            }
            out += t[i]; i++
        }
        return out
    }

    companion object {
        /** Lower-case, apostrophes dropped ("what's" -> "whats"), other punctuation -> space. */
        fun tokens(text: String): List<String> = text.lowercase()
            .replace("'", "").replace("’", "")
            .map { if (it.isLetterOrDigit()) it else ' ' }
            .joinToString("")
            .split(' ').filter { it.isNotEmpty() }

        private fun indexOf(hay: List<String>, needle: List<String>): Int {
            if (needle.isEmpty() || needle.size > hay.size) return -1
            for (i in 0..hay.size - needle.size) {
                if (needle.indices.all { hay[i + it] == needle[it] }) return i
            }
            return -1
        }

        private fun pow(b: Float, e: Int): Float { var r = 1f; repeat(e) { r *= b }; return r }
    }
}
