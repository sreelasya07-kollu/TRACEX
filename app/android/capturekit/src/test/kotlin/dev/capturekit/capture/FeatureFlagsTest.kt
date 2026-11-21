package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FeatureFlagsTest {
    @Test fun understandingLevelParsing() {
        assertNull(FeatureFlags.understandingLevel(null))
        assertNull(FeatureFlags.understandingLevel(""))
        assertEquals(0, FeatureFlags.understandingLevel("0"))
        assertEquals(1, FeatureFlags.understandingLevel("1"))
        assertEquals(2, FeatureFlags.understandingLevel(" 2\n"))
        assertNull(FeatureFlags.understandingLevel("3"))
        assertNull(FeatureFlags.understandingLevel("on"))
        assertNull(FeatureFlags.understandingLevel("-1"))
    }

    /** ADR 0032 (supersedes ADR 0030 "detector default on"): the detector is OFF unless the property is exactly "1". */
    @Test fun detectorIsOffByDefaultAndOnlyOneTurnsItOn() {
        assertFalse(FeatureFlags.detectorOn(null)); assertFalse(FeatureFlags.detectorOn("")); assertTrue(FeatureFlags.detectorOn("1"))
        assertTrue(FeatureFlags.detectorOn(" 1\n")); assertFalse(FeatureFlags.detectorOn("true")); assertFalse(FeatureFlags.detectorOn("junk"))
        assertFalse(FeatureFlags.detectorOn("0")); assertFalse(FeatureFlags.detectorOn(" 0\n"))
    }

    @Test fun switchParsingKeepsTheDefaultForUnsetBlankOrJunk() {
        for (default in listOf(false, true)) {
            for (raw in listOf(null, "", "  ", "2", "-1", "on", "off", "true", "yes", "01", "1.0")) {
                assertEquals("raw=$raw default=$default", default, FeatureFlags.switchOn(raw, default))
            }
            assertTrue(FeatureFlags.switchOn("1", default)); assertTrue(FeatureFlags.switchOn(" 1\n", default))
            assertFalse(FeatureFlags.switchOn("0", default)); assertFalse(FeatureFlags.switchOn("\t0 ", default))
        }
    }

    @Test fun defaultsAreAllOffWithoutAnyProperty() {
        val d = FeatureFlags.Flags.DEFAULTS
        assertFalse(d.guidance); assertFalse(d.detector); assertFalse(d.voiceCommands); assertFalse(d.recalibrateCard); assertFalse(d.segmentation)
        assertFalse(d.recordImu)   // lean bundle by default (ADR 0034)
        assertNull(d.understanding)
        // Release build (no props read) and a debuggable build with every property unset / junk give the defaults.
        assertEquals(d, FeatureFlags.fromProps { null })
        assertEquals(d, FeatureFlags.fromProps { "" })
        assertEquals(d, FeatureFlags.fromProps { "junk" })
    }

    @Test fun eachPropertyTurnsExactlyItsOwnFeatureOn() {
        fun only(prop: String) = FeatureFlags.fromProps { if (it == prop) "1" else null }
        assertEquals(FeatureFlags.Flags.DEFAULTS.copy(guidance = true), only(FeatureFlags.PROP_GUIDANCE))
        assertEquals(FeatureFlags.Flags.DEFAULTS.copy(detector = true), only(FeatureFlags.PROP_DETECTOR))
        assertEquals(FeatureFlags.Flags.DEFAULTS.copy(voiceCommands = true), only(FeatureFlags.PROP_VOICE_COMMANDS))
        assertEquals(FeatureFlags.Flags.DEFAULTS.copy(recalibrateCard = true), only(FeatureFlags.PROP_RECALIBRATE_CARD))
        assertEquals(FeatureFlags.Flags.DEFAULTS.copy(understanding = 1), only(FeatureFlags.PROP_UNDERSTANDING))
        assertEquals(FeatureFlags.Flags.DEFAULTS.copy(segmentation = true), only(FeatureFlags.PROP_SEGMENTATION))
        assertEquals(FeatureFlags.Flags.DEFAULTS.copy(recordImu = true), only(FeatureFlags.PROP_RECORD_IMU))
        val all = FeatureFlags.fromProps { if (it == FeatureFlags.PROP_UNDERSTANDING) "2" else "1" }
        assertEquals(FeatureFlags.Flags(understanding = 2, detector = true, guidance = true, voiceCommands = true, recalibrateCard = true, segmentation = true, recordImu = true), all)
        val names = listOf(FeatureFlags.PROP_GUIDANCE, FeatureFlags.PROP_DETECTOR, FeatureFlags.PROP_VOICE_COMMANDS,
            FeatureFlags.PROP_RECALIBRATE_CARD, FeatureFlags.PROP_UNDERSTANDING, FeatureFlags.PROP_SEGMENTATION,
            FeatureFlags.PROP_RECORD_IMU)
        assertEquals(names.size, names.toSet().size)
        for (n in names) assertTrue(n, n.startsWith("debug.capturekit."))
    }

    @Test fun understandingDefaultsToObjectsOnlyWhenTheDetectorRuns() {
        assertEquals(1, FeatureFlags.understandingFor(null, detectorWillRun = true))
        assertNull(FeatureFlags.understandingFor(null, detectorWillRun = false))   // no detector: session unchanged (no key)
        // An explicit property always wins, including 0 (off) and 2 (gap steering), with or without the detector.
        assertEquals(0, FeatureFlags.understandingFor(0, detectorWillRun = true))
        assertEquals(2, FeatureFlags.understandingFor(2, detectorWillRun = true))
        assertEquals(2, FeatureFlags.understandingFor(2, detectorWillRun = false))
        assertEquals(mapOf<String, Any>("guide.understanding" to 1), FeatureFlags.extraConfig(FeatureFlags.understandingFor(null, true)))
        assertEquals(emptyMap<String, Any>(), FeatureFlags.extraConfig(FeatureFlags.understandingFor(null, false)))
    }

    @Test fun extraConfigIsPassedExplicitlyWhenSetAndEmptyWhenUnset() {
        assertEquals(emptyMap<String, Any>(), FeatureFlags.extraConfig(null))
        assertEquals(mapOf<String, Any>("guide.understanding" to 0), FeatureFlags.extraConfig(0))
        assertEquals(mapOf<String, Any>("guide.understanding" to 1), FeatureFlags.extraConfig(1))
        assertEquals(mapOf<String, Any>("guide.understanding" to 2), FeatureFlags.extraConfig(2))
    }

    /** ADR 0034: the lean bundle is the core default, so `record_imu` is sent only when research switches it on. */
    @Test fun recordImuIsSentOnlyWhenSwitchedOn() {
        assertFalse(FeatureFlags.extraConfig(null).containsKey("record_imu"))
        assertFalse(FeatureFlags.extraConfig(1, recordImu = false).containsKey("record_imu"))
        assertEquals(mapOf<String, Any>("record_imu" to 1), FeatureFlags.extraConfig(null, recordImu = true))
        assertEquals(mapOf<String, Any>("guide.understanding" to 1, "record_imu" to 1), FeatureFlags.extraConfig(1, true))
        val f = FeatureFlags.fromProps { if (it == FeatureFlags.PROP_RECORD_IMU) "1" else null }
        assertTrue(f.recordImu)
        assertEquals(mapOf<String, Any>("record_imu" to 1), FeatureFlags.extraConfig(f.understanding, f.recordImu))
    }

    /**
     * Brief step 3: with the default flags (detector off, no understanding override) the session config has no
     * `guide.understanding` key, so the core keeps understanding off and writes no /ar/detections and no /guide/ objects.
     */
    @Test fun defaultFlagsGiveASessionConfigWithoutUnderstanding() {
        val d = FeatureFlags.fromProps { null }
        val detectorWillRun = d.detector && true   // even with the model packaged
        assertFalse(detectorWillRun)
        val understanding = FeatureFlags.understandingFor(d.understanding, detectorWillRun)
        assertNull(understanding)
        assertEquals(emptyMap<String, Any>(), FeatureFlags.extraConfig(understanding))
        assertFalse(FeatureFlags.extraConfig(understanding).containsKey("guide.understanding"))
    }

    @Test fun getpropOutputIsParsedToTheCaptureKitEntriesOnly() {
        val out = """
            [ro.build.version.sdk]: [34]
            [debug.capturekit.guidance]: [1]
            [debug.capturekit.detector]: [0]
            [debug.capturekit.understanding]: []
            garbage line
            [debug.capturekit.voice_commands]: [ 1 ]
            [debug.other.guidance]: [1]
        """.trimIndent()
        val m = FeatureFlags.parseGetprop(out)
        assertEquals(mapOf("debug.capturekit.guidance" to "1", "debug.capturekit.detector" to "0",
            "debug.capturekit.understanding" to "", "debug.capturekit.voice_commands" to " 1 "), m)
        assertEquals(FeatureFlags.Flags.DEFAULTS.copy(guidance = true, voiceCommands = true), FeatureFlags.fromProps { m[it] })
        assertEquals(emptyMap<String, String>(), FeatureFlags.parseGetprop(""))
        assertEquals(FeatureFlags.Flags.DEFAULTS, FeatureFlags.fromProps { FeatureFlags.parseGetprop("junk\n[x]")[it] })
    }

    @Test fun safetyPromptsTimeOutOnlyWithGuidanceOffAndNotWhileRecalibrating() {
        val hold = FeatureFlags.PROMPT_HOLD_MS
        // Guidance off: shown while fresh, then the scan prompt takes over.
        assertTrue(FeatureFlags.instructionStillShown("More light", 1_000, 1_000, guidance = false, recalibrating = false))
        assertTrue(FeatureFlags.instructionStillShown("More light", 1_000, 1_000 + hold - 1, guidance = false, recalibrating = false))
        assertFalse(FeatureFlags.instructionStillShown("More light", 1_000, 1_000 + hold, guidance = false, recalibrating = false))
        // RECALIBRATE stays while the core is recovering; guidance on keeps the old "until the next one" behaviour.
        assertTrue(FeatureFlags.instructionStillShown("Turn slowly", 0, 60_000, guidance = false, recalibrating = true))
        assertTrue(FeatureFlags.instructionStillShown("Turn left", 0, 60_000, guidance = true, recalibrating = false))
        // Nothing to show.
        assertFalse(FeatureFlags.instructionStillShown("", 0, 0, guidance = true, recalibrating = true))
    }

    /** Guidance gate over all 15 core InstrType names (instruction.cpp type_name, append-only order). */
    @Test fun guidanceGateTable() {
        val table = listOf(
            "SLOW_DOWN" to true, "HOLD_STILL" to true, "RELOCALIZE" to true, "MOVE_TO" to false, "ROTATE_TOWARD" to false,
            "TILT_UP" to false, "TILT_DOWN" to false, "STEP_BACK" to false, "SWEEP" to false, "MORE_LIGHT" to true,
            "FINISH" to false, "PROGRESS" to false, "KEEP_TURNING" to false, "AIM_TEXTURED" to true, "RECALIBRATE" to true,
        )
        assertEquals(15, table.size)
        assertEquals(15, table.map { it.first }.toSet().size)
        for ((type, kept) in table) {
            assertEquals("$type guidance off", kept, FeatureFlags.showInstruction(type, guidance = false))
            assertTrue("$type guidance on", FeatureFlags.showInstruction(type, guidance = true))
        }
        assertEquals(table.filter { it.second }.map { it.first }.toSet(), FeatureFlags.SAFETY_INSTRUCTIONS)
        // Unknown (newer core) types are guidance until they are added to the safety list.
        assertFalse(FeatureFlags.showInstruction("SOMETHING_NEW", guidance = false))
        assertFalse(FeatureFlags.showInstruction("", guidance = false))
        assertFalse(FeatureFlags.showInstruction("slow_down", guidance = false))
    }
}
