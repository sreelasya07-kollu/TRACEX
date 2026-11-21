package dev.capturekit.guidance

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class VoiceIntentTest {
    private val p = VoiceIntentParser(PhraseTables.EN)

    private fun intentOf(text: String): VoiceIntent? = (p.parse(text) as? VoiceParse.Match)?.intent

    @Test fun exactAndNormalisedPhrases() {
        val cases = listOf(
            "pause" to VoiceIntent.PAUSE,
            "Stop." to VoiceIntent.PAUSE,
            "STOP!" to VoiceIntent.PAUSE,
            "hold on" to VoiceIntent.PAUSE,
            "resume" to VoiceIntent.RESUME,
            "Continue, please" to VoiceIntent.RESUME,
            "keep going" to VoiceIntent.RESUME,
            "repeat" to VoiceIntent.REPEAT,
            "Say that again?" to VoiceIntent.REPEAT,
            "What's missing?" to VoiceIntent.MISSING,
            "what is missing" to VoiceIntent.MISSING,
            "How much is left?" to VoiceIntent.MISSING,
            "finish" to VoiceIntent.FINISH,
            "I'm done" to VoiceIntent.FINISH,
            "louder" to VoiceIntent.LOUDER,
            "Speak up!" to VoiceIntent.LOUDER,
            "quieter" to VoiceIntent.QUIETER,
            "too loud" to VoiceIntent.QUIETER,
        )
        for ((text, want) in cases) assertEquals(text, want, intentOf(text))
    }

    @Test fun exactMatchHasFullScore() {
        val m = p.parse("pause") as VoiceParse.Match
        assertEquals(1f, m.score, 1e-6f)
        assertEquals("pause", m.heard)
    }

    @Test fun fillersAreIgnored() {
        val cases = listOf(
            "um pause" to VoiceIntent.PAUSE,
            "uh, okay, resume please" to VoiceIntent.RESUME,
            "hey could you repeat that" to VoiceIntent.REPEAT,
            "erm so what's left" to VoiceIntent.MISSING,
            "can you speak up please" to VoiceIntent.LOUDER,
        )
        for ((text, want) in cases) {
            val m = p.parse(text) as VoiceParse.Match
            assertEquals(text, want, m.intent)
            assertEquals(text, 1f, m.score, 1e-6f) // fillers stripped -> exact
        }
    }

    @Test fun partialPhrasesInsideShortUtterances() {
        val cases = listOf(
            "pause for a second" to VoiceIntent.PAUSE,
            "stop scanning for now" to VoiceIntent.PAUSE,
            "ok let's continue" to VoiceIntent.RESUME,
            "how much is left here" to VoiceIntent.MISSING,
            "say that again for me" to VoiceIntent.REPEAT,
            "a bit louder" to VoiceIntent.LOUDER,
        )
        for ((text, want) in cases) {
            val m = p.parse(text)
            assertTrue("$text -> $m", m is VoiceParse.Match)
            m as VoiceParse.Match
            assertEquals(text, want, m.intent)
            assertTrue(text, m.score < 1f)
        }
    }

    @Test fun noiseAndOutOfVocabularyAreNoMatch() {
        val cases = listOf(
            "" to "empty",
            "   " to "empty",
            "?!." to "empty",
            "um uh please" to "filler_only",
            "asdf qwerty" to "no_phrase",
            "the paws are muddy" to "no_phrase",
            "what time is it" to "no_phrase",
            "pass" to "no_phrase",
        )
        for ((text, reason) in cases) assertEquals(text, VoiceParse.NoMatch(reason), p.parse(text))
    }

    @Test fun backgroundTalkWithManyExtraWordsIsIgnored() {
        assertEquals(
            VoiceParse.NoMatch("no_phrase"),
            p.parse("I told him to pause the music yesterday after dinner"),
        )
    }

    @Test fun negatedCommandsNeverAct() {
        for (text in listOf("don't stop", "do not pause", "not done", "never finish", "no stop")) {
            assertEquals(text, VoiceParse.NoMatch("negated"), p.parse(text))
        }
    }

    @Test fun twoCommandsInOneUtteranceAreAmbiguous() {
        assertEquals(VoiceParse.NoMatch("ambiguous"), p.parse("pause and continue"))
        assertEquals(VoiceParse.NoMatch("ambiguous"), p.parse("louder quieter"))
    }

    @Test fun stopMapsToPauseNeverFinish() {
        for (text in listOf("stop", "stop scanning", "stop the scan")) {
            val m = p.parse(text) as VoiceParse.Match
            assertEquals(text, VoiceIntent.PAUSE, m.intent)
            assertFalse(text, m.needsConfirmation)
        }
    }

    @Test fun finishAlwaysNeedsConfirmationAndNothingElseDoes() {
        for (i in VoiceIntent.entries) assertEquals(i.name, i == VoiceIntent.FINISH, i.requiresConfirmation)
        for (text in listOf("finish", "finish the scan", "I am done", "all done")) {
            val m = p.parse(text) as VoiceParse.Match
            assertEquals(VoiceIntent.FINISH, m.intent)
            assertTrue(text, m.needsConfirmation)
        }
    }

    @Test fun confirmationAnswers() {
        val cases = listOf(
            "yes" to Confirmation.YES,
            "Yeah, go ahead" to Confirmation.UNKNOWN, // two answers glued: not a listed phrase
            "yeah" to Confirmation.YES,
            "go ahead" to Confirmation.YES,
            "yes finish" to Confirmation.YES,
            "no" to Confirmation.NO,
            "not yet" to Confirmation.NO,
            "cancel" to Confirmation.NO,
            "keep scanning" to Confirmation.NO,
            "" to Confirmation.UNKNOWN,
            "maybe" to Confirmation.UNKNOWN,
            "pause" to Confirmation.UNKNOWN,
        )
        for ((text, want) in cases) assertEquals(text, want, p.parseConfirmation(text))
    }

    @Test fun confirmationWithYesAndNoInNBestIsUnknown() {
        assertEquals(
            Confirmation.UNKNOWN,
            p.parseConfirmation(listOf(Hypothesis("yet"), Hypothesis("yes"), Hypothesis("no"))),
        )
        assertEquals(
            Confirmation.UNKNOWN,
            p.parseConfirmation(listOf(Hypothesis("yes", 0.9f), Hypothesis("no", 0.1f))),
        )
    }

    @Test fun confirmationUsesSamePriorAndMinScoreAsParse() {
        assertEquals(Confirmation.UNKNOWN, p.parseConfirmation(listOf(Hypothesis("yes", 0.3f))))
        assertEquals(Confirmation.UNKNOWN, p.parseConfirmation(listOf(Hypothesis("no", 0.3f))))
        assertEquals(Confirmation.YES, p.parseConfirmation(listOf(Hypothesis("yet"), Hypothesis("yes"))))
        // Unreported yes below a reported 0.5 miss: capped at 0.5 x 0.8 = 0.4 < minScore.
        assertEquals(
            Confirmation.UNKNOWN,
            p.parseConfirmation(listOf(Hypothesis("yet", 0.5f), Hypothesis("yes"))),
        )
    }

    @Test fun mixedConfidencesCapUnreportedBelowReportedAbove() {
        val m = p.parse(listOf(Hypothesis("louder", 0.6f), Hypothesis("quieter", 0f))) as VoiceParse.Match
        assertEquals(VoiceIntent.LOUDER, m.intent)
        assertEquals(0.6f, m.score, 1e-6f)
        // Same with null instead of 0.0.
        val n = p.parse(listOf(Hypothesis("louder", 0.6f), Hypothesis("quieter"))) as VoiceParse.Match
        assertEquals(VoiceIntent.LOUDER, n.intent)
    }

    @Test fun apostrophesNormaliseAway() {
        assertEquals(listOf("whats", "next"), VoiceIntentParser.tokens("What's next?"))
        assertEquals(listOf("whats", "next"), VoiceIntentParser.tokens("What’s next?"))
        assertEquals(listOf("whats", "left", "to", "scan"), VoiceIntentParser.tokens("what's left to scan"))
        val cases = listOf(
            "What's next?" to 1f,
            "What’s next?" to 1f,
            "whats next" to 1f,
            "What's left to scan?" to 1f,
            "What’s left to scan" to 1f,
            "so what's next then" to 0.8f,
        )
        for ((text, score) in cases) {
            val m = p.parse(text) as VoiceParse.Match
            assertEquals(text, VoiceIntent.MISSING, m.intent)
            assertEquals(text, score, m.score, 1e-6f)
        }
    }

    @Test fun againAfterACommandIsNotAmbiguous() {
        val cases = listOf(
            "pause again" to VoiceIntent.PAUSE,
            "resume again" to VoiceIntent.RESUME,
            "continue again" to VoiceIntent.RESUME,
            "say that again" to VoiceIntent.REPEAT,
            "go again" to VoiceIntent.RESUME,
        )
        for ((text, want) in cases) assertEquals(text, want, intentOf(text))
        assertEquals(VoiceParse.NoMatch("no_phrase"), p.parse("again"))
    }

    @Test fun nBestPicksMatchingHypothesis() {
        val m = p.parse(
            listOf(Hypothesis("pours", 0.9f), Hypothesis("pause", 0.8f), Hypothesis("paws", 0.4f)),
        ) as VoiceParse.Match
        assertEquals(VoiceIntent.PAUSE, m.intent)
        assertEquals(0.8f, m.score, 1e-6f)
        assertEquals("pause", m.heard)
    }

    @Test fun nBestWithoutConfidenceUsesRankPrior() {
        val m = p.parse(listOf(Hypothesis("pours"), Hypothesis("pause"))) as VoiceParse.Match
        assertEquals(0.8f, m.score, 1e-6f) // rankDecay^1
        // Confidence <= 0 is "not reported", not "zero confidence".
        val z = p.parse(listOf(Hypothesis("pause", 0f))) as VoiceParse.Match
        assertEquals(1f, z.score, 1e-6f)
    }

    @Test fun lowConfidenceIsNoMatch() {
        assertEquals(VoiceParse.NoMatch("low_confidence"), p.parse(listOf(Hypothesis("pause", 0.3f))))
        // Contained (0.8) x 0.5 = 0.4 < 0.45
        assertEquals(VoiceParse.NoMatch("low_confidence"), p.parse(listOf(Hypothesis("pause for a second", 0.5f))))
    }

    @Test fun closeHypothesesOfDifferentIntentsAreAmbiguous() {
        assertEquals(
            VoiceParse.NoMatch("ambiguous"),
            p.parse(listOf(Hypothesis("louder", 0.7f), Hypothesis("quieter", 0.65f))),
        )
        // Clear winner: fine.
        val m = p.parse(listOf(Hypothesis("louder", 0.9f), Hypothesis("quieter", 0.3f))) as VoiceParse.Match
        assertEquals(VoiceIntent.LOUDER, m.intent)
    }

    @Test fun sameIntentAcrossHypothesesIsNotAmbiguous() {
        val m = p.parse(listOf(Hypothesis("pause", 0.7f), Hypothesis("stop", 0.69f))) as VoiceParse.Match
        assertEquals(VoiceIntent.PAUSE, m.intent)
    }

    @Test fun emptyNBestIsNoMatch() {
        assertEquals(VoiceParse.NoMatch("empty"), p.parse(emptyList()))
    }

    @Test fun localeLookup() {
        assertNotNull(PhraseTables.forLocale("en"))
        assertEquals("en", PhraseTables.forLocale("en-US")?.locale)
        assertEquals("en", PhraseTables.forLocale("en_GB")?.locale)
        assertNull(PhraseTables.forLocale("hi-IN")) // not shipped yet: shell hides the mic
    }

    @Test fun everyIntentHasEnglishPhrases() {
        for (i in VoiceIntent.entries) {
            assertTrue(i.name, PhraseTables.EN.phrases[i].orEmpty().isNotEmpty())
            // Each listed phrase parses back to its own intent.
            for (ph in PhraseTables.EN.phrases.getValue(i)) assertEquals(ph, i, intentOf(ph))
        }
    }

    @Test fun phraseUnderTwoIntentsIsRejectedAtConstruction() {
        val bad = PhraseTables.EN.copy(
            phrases = PhraseTables.EN.phrases + (VoiceIntent.QUIETER to listOf("pause")),
        )
        try {
            VoiceIntentParser(bad); throw AssertionError("expected IllegalArgumentException")
        } catch (_: IllegalArgumentException) {
        }
    }

    @Test fun newLocaleNeedsOnlyData() {
        val toy = PhraseTable(
            locale = "xx",
            phrases = mapOf(VoiceIntent.PAUSE to listOf("ruk"), VoiceIntent.RESUME to listOf("chalo")),
            fillers = listOf("zara"),
            negations = listOf("mat"),
            yes = listOf("haan"),
            no = listOf("nahin"),
        )
        val q = VoiceIntentParser(toy)
        assertEquals(VoiceIntent.PAUSE, (q.parse("zara ruk") as VoiceParse.Match).intent)
        assertEquals(VoiceParse.NoMatch("negated"), q.parse("ruk mat"))
        assertEquals(Confirmation.YES, q.parseConfirmation("haan"))
    }
}
