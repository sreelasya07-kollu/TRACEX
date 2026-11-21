package dev.capturekit.capture

import dev.capturekit.guidance.SpeechScheduler
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class VoiceStatsTest {
    @Test fun countsPromptsDropsAndRepeatsFromRealScheduler() {
        val sch = SpeechScheduler(SpeechScheduler.Config(minGapMs = 0, repeatWindowMs = 10_000))
        val st = VoiceStats()
        st.begin(0)
        st.onAction(sch.onInstruction(1, "Turn left.", 2, 0), 0)             // speak
        st.onTtsStart("ck-1", 300); sch.onDone("ck-1", 1_000)
        st.onAction(sch.onInstruction(2, "Turn left.", 2, 2_000), 2_000)     // repeat (same text in window)
        st.onAction(sch.onInstruction(3, "Move forward.", 1, 3_000), 3_000)  // speak
        st.onAction(sch.onInstruction(4, "Step back.", 1, 3_100), 3_100)     // queued (same priority)
        st.onAction(sch.onInstruction(5, "Hm.", 0, 3_200), 3_200)            // busy
        st.onTtsStart("ck-3", 3_500)
        st.onRepeatRequest()
        val s = st.snapshot(60_000)
        assertEquals(2, s.prompts); assertEquals(1, s.queued); assertEquals(1, s.repeatRequests)
        assertEquals(mapOf("repeat" to 1, "busy" to 1), s.drops); assertEquals(1, s.repeatsSuppressed)
        assertEquals(2.0, s.promptsPerMin, 1e-9)
        assertEquals(400L, s.ttsStartMedianMs)   // (300 + 500) / 2
        assertTrue(s.withinBudget(SpeechScheduler.Config()))
    }

    @Test fun budgetCheckUsesMaxPerMinute() {
        val st = VoiceStats(); st.begin(0)
        repeat(10) { st.onAction(SpeechScheduler.Action.Speak("ck-$it", "x$it", 2, false), it * 1_000L) }
        val s = st.snapshot(60_000)
        assertEquals(10.0, s.promptsPerMin, 1e-9)
        assertFalse(s.withinBudget(SpeechScheduler.Config(maxPerMinute = 8)))
        assertTrue(s.withinBudget(SpeechScheduler.Config(maxPerMinute = 10)))
    }

    @Test fun defaultBudgetIsSixPerMinute() {
        assertEquals(6, SpeechScheduler.Config().maxPerMinute)
        val st = VoiceStats(); st.begin(0)
        repeat(6) { st.onAction(SpeechScheduler.Action.Speak("ck-$it", "x$it", 2, false), it * 1_000L) }
        assertTrue(st.snapshot(60_000).withinBudget(SpeechScheduler.Config()))
        st.onAction(SpeechScheduler.Action.Speak("ck-7", "x7", 2, false), 7_000L)
        assertFalse(st.snapshot(60_000).withinBudget(SpeechScheduler.Config()))
    }

    @Test fun budgetCountsOnlyNonUrgentPrompts() {
        val st = VoiceStats(); st.begin(0)
        repeat(7) { st.onAction(SpeechScheduler.Action.Speak("ck-$it", "n$it", 2, false), it * 1_000L) }
        repeat(5) { st.onAction(SpeechScheduler.Action.Speak("ck-u$it", "u$it", 3, true), 10_000L + it) }
        val s = st.snapshot(60_000)
        assertEquals(12.0, s.promptsPerMin, 1e-9)
        assertEquals(mapOf(2 to 7, 3 to 5), s.promptsByPriority)
        assertEquals(7.0, s.nonUrgentPerMin(3), 1e-9)
        assertTrue(s.withinBudget(SpeechScheduler.Config(maxPerMinute = 8)))
        assertFalse(s.withinBudget(SpeechScheduler.Config(maxPerMinute = 8, urgentPriority = 4)))  // now all count
    }

    @Test fun ratesDivideByAtLeastOneMinute() {
        val st = VoiceStats(); st.begin(0)
        st.onAction(SpeechScheduler.Action.Speak("ck-1", "a", 2, false), 0)
        val s = st.snapshot(5_000)
        assertEquals(1.0, s.promptsPerMin, 1e-9)   // not 12/min
        assertTrue(s.withinBudget(SpeechScheduler.Config()))
    }

    @Test fun staleQueuedLinesAreCountedViaSchedulerHook() {
        val sch = SpeechScheduler(SpeechScheduler.Config(minGapMs = 0, pendingTtlMs = 1_000))
        val st = VoiceStats(); sch.onStale = st::onStale
        st.onAction(sch.onInstruction(1, "One.", 2, 0), 0)
        st.onAction(sch.onInstruction(2, "Two.", 2, 10), 10)       // queued
        st.onAction(sch.onInstruction(3, "Three.", 2, 20), 20)     // queued, replaces Two -> stale
        st.onAction(sch.onInstruction(4, "Urgent.", 3, 30), 30)    // flushes, drops pending Three -> stale
        st.onAction(sch.onInstruction(5, "Five.", 3, 40), 40)      // queued behind the urgent line
        sch.onDone("ck-4", 2_000); st.onUtteranceEnd("ck-4")
        st.onAction(sch.nextPending(2_000), 2_000)                 // Five is past its TTL -> stale
        assertEquals(3, st.snapshot(60_000).drops[VoiceStats.STALE])
    }

    @Test fun issuedAtIsForgottenOnEndAndCapped() {
        val st = VoiceStats(maxLatencySamples = 2)
        st.onAction(SpeechScheduler.Action.Speak("ck-1", "a", 2, false), 0)
        st.onUtteranceEnd("ck-1"); st.onTtsStart("ck-1", 100)        // ended before start: no sample
        assertNull(st.snapshot(1_000).ttsStartMedianMs)
        listOf("ck-2", "ck-3", "ck-4").forEachIndexed { i, id -> st.onAction(SpeechScheduler.Action.Speak(id, id, 2, false), i * 10L) }
        st.onTtsStart("ck-2", 500)                                   // evicted by the cap: no sample
        st.onTtsStart("ck-4", 70)
        assertEquals(50L, st.snapshot(1_000).ttsStartMedianMs)
    }

    @Test fun ttsStartIgnoresUnknownAndDuplicateIds() {
        val st = VoiceStats()
        st.onAction(SpeechScheduler.Action.Speak("ck-1", "a", 2, false), 100)
        st.onTtsStart("ck-9", 200); st.onTtsStart("ck-1", 250); st.onTtsStart("ck-1", 900)
        assertEquals(150L, st.snapshot(1_000).ttsStartMedianMs)
    }

    @Test fun emptySessionIsZeroAndNullMedian() {
        val s = VoiceStats().snapshot(5_000)
        assertEquals(0, s.prompts); assertEquals(0.0, s.promptsPerMin, 0.0); assertNull(s.ttsStartMedianMs)
        assertEquals(
            "{\"prompts\":0,\"prompts_by_priority\":{},\"queued\":0,\"drops\":{},\"repeats_suppressed\":0,\"repeat_requests\":0," +
                "\"duration_ms\":0,\"prompts_per_min\":0.00,\"tts_start_median_ms\":null}",
            s.toJson(),
        )
    }

    @Test fun jsonAndLogLineAreStable() {
        val st = VoiceStats(); st.begin(0)
        st.onAction(SpeechScheduler.Action.Speak("ck-1", "a", 2, false), 0); st.onTtsStart("ck-1", 120)
        st.onAction(SpeechScheduler.Action.Drop("gap"), 10); st.onAction(SpeechScheduler.Action.Drop("budget"), 20)
        val s = st.snapshot(30_000)
        assertEquals("prompts 1 queued 0 drops budget=1,gap=1 repeatReq 0 perMin 1.00 ttsStartMedMs 120", s.logLine())
        assertTrue(s.toJson().contains("\"drops\":{\"budget\":1,\"gap\":1}"))
        assertTrue(s.toJson().contains("\"prompts_per_min\":1.00"))
        assertTrue(s.toJson().contains("\"prompts_by_priority\":{\"2\":1}"))
    }

    @Test fun resetClearsEverything() {
        val st = VoiceStats(); st.begin(0); st.onAction(SpeechScheduler.Action.Drop("muted"), 0); st.onRepeatRequest()
        st.reset()
        val s = st.snapshot(10_000)
        assertEquals(0, s.repeatRequests); assertTrue(s.drops.isEmpty()); assertEquals(0L, s.durationMs)
    }

    @Test fun medianOddEven() {
        assertEquals(3L, VoiceStats.median(listOf(5, 1, 3)))
        assertEquals(2L, VoiceStats.median(listOf(1, 3)))
        assertNull(VoiceStats.median(emptyList()))
    }

    private class MapStore(val m: MutableMap<String, Long> = HashMap()) : LongStore {
        override fun getLong(key: String) = m[key]
        override fun putLongs(values: Map<String, Long>) { m.putAll(values) }
    }

    @Test fun budgetRoundTripsThroughStore() {
        val st = MapStore()
        val cfg = SpeechScheduler.Config(minGapMs = 1_500, maxPerMinute = 7, repeatWindowMs = 20_000, pendingTtlMs = 3_000, urgentPriority = 3, speakTimeoutMs = 8_000)
        SpeechBudgetPrefs.save(st, cfg)
        assertEquals(cfg, SpeechBudgetPrefs.load(st))
    }

    @Test fun budgetMissingOrInvalidFallsBackPerField() {
        assertEquals(SpeechScheduler.Config(), SpeechBudgetPrefs.load(MapStore()))
        val st = MapStore(mutableMapOf(
            SpeechBudgetPrefs.MAX_PER_MINUTE to 0L,                     // invalid: < 1
            SpeechBudgetPrefs.MIN_GAP_MS to -5L,                        // invalid: negative
            SpeechBudgetPrefs.URGENT_PRIORITY to Long.MAX_VALUE,        // invalid: > Int range
            SpeechBudgetPrefs.REPEAT_WINDOW_MS to 5_000L,               // valid
        ))
        val d = SpeechScheduler.Config()
        assertEquals(d.copy(repeatWindowMs = 5_000), SpeechBudgetPrefs.load(st))
    }
}
