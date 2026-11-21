package dev.capturekit.guidance

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SpeechSchedulerTest {
    private fun s(cfg: SpeechScheduler.Config = SpeechScheduler.Config()) = SpeechScheduler(cfg)
    private fun sp(a: SpeechScheduler.Action) = a as SpeechScheduler.Action.Speak

    @Test fun speaksWhenIdle() {
        val a = sp(s().onInstruction(1, "Turn slowly left.", 2, 0))
        assertEquals("Turn slowly left.", a.text); assertEquals("ck-1", a.utteranceId); assertTrue(!a.flush)
    }

    @Test fun defaultSpeaksAtMostSixNonUrgentPerMinute() {
        val x = s(SpeechScheduler.Config(minGapMs = 0, repeatWindowMs = 0))
        var spoken = 0
        for (i in 1..8) {
            val t = i * 5_000L
            if (x.onInstruction(i, "Line $i.", 2, t) is SpeechScheduler.Action.Speak) { spoken++; x.onDone("ck-$i", t + 100) }
        }
        assertEquals(6, spoken)
    }

    @Test fun mutedDropsEverything() {
        val x = s(); x.enabled = false
        assertEquals(SpeechScheduler.Action.Drop("muted"), x.onInstruction(1, "Slow down.", 3, 0))
    }

    @Test fun sameIdIsNeverRepeated() {
        val x = s(); x.onInstruction(1, "Move forward.", 2, 0); x.onDone("ck-1", 1000)
        assertEquals(SpeechScheduler.Action.Drop("repeat"), x.onInstruction(1, "Move forward.", 2, 60_000))
    }

    @Test fun sameTextWithinWindowIsSuppressedButAllowedAfter() {
        val x = s(SpeechScheduler.Config(repeatWindowMs = 10_000, minGapMs = 0))
        x.onInstruction(1, "Slow down.", 3, 0); x.onDone("ck-1", 500)
        assertEquals(SpeechScheduler.Action.Drop("repeat"), x.onInstruction(2, "Slow down.", 3, 5_000))
        assertTrue(x.onInstruction(3, "Slow down.", 3, 11_000) is SpeechScheduler.Action.Speak)
    }

    @Test fun higherPriorityPreemptsWithFlush() {
        val x = s(); x.onInstruction(1, "Move to the left wall.", 2, 0); x.onStart("ck-1", 100)
        val a = sp(x.onInstruction(2, "Slow down.", 3, 600))
        assertTrue(a.flush)
    }

    @Test fun lowerPriorityDuringSpeechIsDropped() {
        val x = s(); x.onInstruction(1, "Slow down.", 3, 0); x.onStart("ck-1", 100)
        assertEquals(SpeechScheduler.Action.Drop("busy"), x.onInstruction(2, "More light please.", 1, 600))
    }

    @Test fun equalPriorityQueuesNewestAndReleasesAfterDone() {
        val x = s(SpeechScheduler.Config(minGapMs = 0))
        x.onInstruction(1, "Tilt up.", 2, 0); x.onStart("ck-1", 100)
        assertEquals(SpeechScheduler.Action.Queued, x.onInstruction(2, "Step back.", 2, 500))
        assertEquals(SpeechScheduler.Action.Queued, x.onInstruction(3, "Turn left.", 2, 900))
        x.onDone("ck-1", 1200)
        val n = sp(x.nextPending(1300)!!)
        assertEquals("Turn left.", n.text)
        assertNull(x.nextPending(1400))
    }

    @Test fun pendingExpires() {
        val x = s(SpeechScheduler.Config(minGapMs = 0, pendingTtlMs = 3_000))
        x.onInstruction(1, "Tilt up.", 2, 0); x.onStart("ck-1", 100)
        x.onInstruction(2, "Step back.", 2, 500); x.onDone("ck-1", 1000)
        assertNull(x.nextPending(4_000))
    }

    @Test fun lateDoneOfFlushedUtteranceDoesNotClearSuccessor() {
        val x = s(); x.onInstruction(1, "Tilt up.", 2, 0); x.onStart("ck-1", 100)
        x.onInstruction(2, "Slow down.", 3, 600)
        x.onDone("ck-1", 700)
        assertTrue(x.isSpeaking())
    }

    @Test fun budgetLimitsNonUrgentButNotUrgent() {
        val x = s(SpeechScheduler.Config(minGapMs = 0, maxPerMinute = 2))
        for (i in 1..2) { assertTrue(x.onInstruction(i, "Hint $i.", 2, i * 1000L) is SpeechScheduler.Action.Speak); x.onDone("ck-$i", i * 1000L + 100) }
        assertEquals(SpeechScheduler.Action.Drop("budget"), x.onInstruction(3, "Hint 3.", 2, 4_000))
        assertTrue(x.onInstruction(4, "Slow down.", 3, 4_500) is SpeechScheduler.Action.Speak)
        x.onDone("ck-4", 5_000)
        assertTrue(x.onInstruction(5, "Hint 5.", 2, 70_000) is SpeechScheduler.Action.Speak)
    }

    @Test fun minGapAppliesToNonUrgentOnly() {
        val x = s(SpeechScheduler.Config(minGapMs = 2_000))
        x.onInstruction(1, "Hint one.", 2, 0); x.onDone("ck-1", 500)
        assertEquals(SpeechScheduler.Action.Drop("gap"), x.onInstruction(2, "Hint two.", 2, 1_000))
        assertTrue(x.onInstruction(3, "Slow down.", 3, 1_000) is SpeechScheduler.Action.Speak)
    }

    @Test fun engineThatNeverCallsBackDoesNotStickSpeakingForever() {
        val x = s(SpeechScheduler.Config(minGapMs = 0, speakTimeoutMs = 5_000))
        x.onInstruction(1, "Tilt up.", 2, 0)            // engine never calls onStart/onDone/onError
        assertTrue(x.isSpeaking())
        assertEquals(SpeechScheduler.Action.Drop("busy"), x.onInstruction(2, "More light please.", 1, 4_000))
        assertTrue(!x.expireStuck(4_999))
        assertTrue(x.expireStuck(5_000))
        assertTrue(!x.isSpeaking())
        assertTrue(x.onInstruction(3, "Step back.", 2, 5_100) is SpeechScheduler.Action.Speak)
    }

    @Test fun onInstructionSelfHealsAfterTimeoutWithoutExplicitTick() {
        val x = s(SpeechScheduler.Config(minGapMs = 0, speakTimeoutMs = 5_000))
        x.onInstruction(1, "Tilt up.", 2, 0)
        assertTrue(x.onInstruction(2, "Step back.", 2, 6_000) is SpeechScheduler.Action.Speak)
    }

    @Test fun startCallbackRestartsTheTimeout() {
        val x = s(SpeechScheduler.Config(speakTimeoutMs = 5_000))
        x.onInstruction(1, "Tilt up.", 2, 0); x.onStart("ck-1", 3_000)
        assertTrue(!x.expireStuck(7_000))
        assertTrue(x.expireStuck(8_000))
    }

    @Test fun blankTextIsDropped() {
        assertEquals(SpeechScheduler.Action.Drop("empty"), s().onInstruction(1, "  ", 2, 0))
    }

    @Test fun repeatLastWithNothingSaidDrops() {
        assertEquals(SpeechScheduler.Action.Drop("nothing"), s().repeatLast(0))
    }

    @Test fun repeatLastBypassesRepeatWindowGapAndBudget() {
        val x = s(SpeechScheduler.Config(maxPerMinute = 1, minGapMs = 60_000))
        x.onInstruction(1, "Turn slowly left.", 2, 0); x.onDone("ck-1", 500)
        // Same text 1 s later: the guidance path suppresses it, the user's "repeat" does not.
        assertEquals(SpeechScheduler.Action.Drop("repeat"), x.onInstruction(2, "Turn slowly left.", 2, 1_000))
        val a = sp(x.repeatLast(1_000))
        assertEquals("Turn slowly left.", a.text)
        assertTrue(a.utteranceId != "ck-1")
        assertTrue(!a.flush)
    }

    @Test fun repeatLastCutsCurrentSpeechAndSurvivesReset() {
        val x = s()
        x.onInstruction(1, "Step back.", 2, 0); x.onStart("ck-1", 100)
        x.reset()   // pause: stopNow() resets the scheduler
        assertEquals("Step back.", sp(x.repeatLast(2_000)).text)
        val b = sp(x.repeatLast(2_500))   // still speaking the first repeat: flush it
        assertTrue(b.flush)
        assertEquals("Step back.", b.text)
    }

    @Test fun repeatLastWhenMutedDrops() {
        val x = s(); x.onInstruction(1, "Step back.", 2, 0); x.onDone("ck-1", 300)
        x.enabled = false
        assertEquals(SpeechScheduler.Action.Drop("muted"), x.repeatLast(1_000))
    }

    @Test fun speakNowFeedbackIsNotRemembered() {
        val x = s(); x.onInstruction(1, "Step back.", 2, 0); x.onDone("ck-1", 300)
        sp(x.speakNow("Louder", 1_000, remember = false))
        assertEquals("Step back.", x.lastText)
        assertEquals("Step back.", sp(x.repeatLast(1_100)).text)
    }

    @Test fun speakNowDoesNotConsumeTheBudget() {
        val x = s(SpeechScheduler.Config(maxPerMinute = 1, minGapMs = 0))
        sp(x.speakNow("Missing: Wall 2, 40 percent.", 0)); x.onDone("ck--1", 100)
        assertTrue(x.onInstruction(5, "Turn left.", 2, 200) is SpeechScheduler.Action.Speak)
    }
}
