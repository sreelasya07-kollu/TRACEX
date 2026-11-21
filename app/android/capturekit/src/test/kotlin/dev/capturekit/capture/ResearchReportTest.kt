package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class ResearchReportTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun input(trial: TrialSetup = TrialSetup("P01", TrialArm.GUIDED, "Living room")): ResearchReport.Input {
        val vs = VoiceStats(); vs.begin(0)
        vs.onAction(dev.capturekit.guidance.SpeechScheduler.Action.Speak("ck-1", "Turn left.", 2, false), 0)
        vs.onTtsStart("ck-1", 250)
        vs.onRepeatRequest()
        return ResearchReport.Input(
            bundleName = "scan_20260101_120000", trial = trial, device = "Pixel 8", appVersion = "0.0.1",
            durationMs = 90_000, keyframes = 41, finishStatus = 0,
            ingest = mapOf("ar_frames" to 100L, "frames_ingested" to 33L),
            thermalMaxLevel = 2, thermalChanges = listOf(0L to 0, 30_000L to 2),
            instructionsEmitted = 7, voice = vs.snapshot(90_000), intents = mapOf("MISSING" to 2, "PAUSE" to 1),
        )
    }

    @Test fun jsonHasTheKeysEmittedByE2eAndIsStable() {
        val j = ResearchReport.toJson(input())
        assertTrue(j.startsWith("{\"schema\":\"research/1\",\"bundle\":\"scan_20260101_120000\",\"participant\":\"P01\""))
        for (k in listOf("\"arm\":\"guided\"", "\"room\":\"Living room\"", "\"device\":\"Pixel 8\"", "\"duration_ms\":90000",
            "\"ingest\":{\"ar_frames\":100,\"frames_ingested\":33}", "\"thermal\":{\"max_level\":2,\"max_level_name\":\"REDUCE_ML\"",
            "{\"t_ms\":30000,\"level\":2}", "\"instructions_emitted\":7", "\"voice_stats\":{\"prompts\":1", "\"repeat_requests\":1",
            "\"tts_start_median_ms\":250", "\"intents\":{\"MISSING\":2,\"PAUSE\":1}")) {
            assertTrue("missing $k in $j", j.contains(k))
        }
        assertEquals(j, ResearchReport.toJson(input()))   // deterministic
        assertEquals(j.count { it == '{' }, j.count { it == '}' })   // balanced braces
    }

    @Test fun escapesQuotesAndControlCharacters() {
        val j = ResearchReport.toJson(input(TrialSetup("P01", TrialArm.UNGUIDED, "a\"b\\c\nd")))
        assertTrue(j.contains("\"room\":\"a\\\"b\\\\c\\nd\""))
        assertTrue(j.contains("\"arm\":\"unguided\""))
    }

    @Test fun nullMedianWhenNoSpeech() {
        val vs = VoiceStats()
        val j = ResearchReport.toJson(input().copy(voice = vs.snapshot(0)))
        assertTrue(j.contains("\"tts_start_median_ms\":null"))
    }

    @Test fun fileIsSiblingOfTheBundleNeverInside() {
        val bundle = File(tmp.newFolder("bundles"), "scan_x").apply { mkdirs() }
        val f = ResearchReport.fileFor(bundle)
        assertEquals(bundle.parentFile.canonicalPath, f.parentFile.canonicalPath)
        assertEquals("scan_x.research.json", f.name)
        assertTrue(ResearchReport.write(bundle, "{\"a\":1}"))
        assertEquals("{\"a\":1}", f.readText())
        assertEquals(0, bundle.list()!!.size)                        // nothing written into the bundle
        assertFalse(File(f.parentFile, f.name + ".tmp").exists())
        assertTrue(ResearchReport.write(bundle, "{\"a\":2}"))         // overwrite works
        assertEquals("{\"a\":2}", f.readText())
    }

    @Test fun deleteSiblingsRemovesResearchAndTmpOnly() {
        val root = tmp.newFolder("b2")
        val bundle = File(root, "scan_y").apply { mkdirs() }
        val other = File(root, "scan_z.research.json").apply { writeText("{}") }
        ResearchReport.write(bundle, "{}")
        File(root, "scan_y.research.json.tmp").writeText("x")
        ResearchReport.deleteSiblings(bundle)
        assertEquals(listOf("scan_y", "scan_z.research.json"), root.list()!!.sorted())
        assertTrue(other.exists())
        assertTrue(ResearchReport.isResearchFile("scan_y.research.json") && ResearchReport.isResearchFile("scan_y.research.json.tmp"))
        assertFalse(ResearchReport.isResearchFile("scan_y"))
    }

    @Test fun ingestStatsCarriesPolicyLevelForThermalSeed() {
        val s = IngestStats(); s.policyLevel = 4
        val t = ThermalTrace(); t.onLevel(s.policyLevel, 0)
        assertEquals(4, t.maxLevel())
    }

    @Test fun trialInputValidation() {
        assertEquals("P-01_a", TrialSetup.cleanCode("  P-01_a "))
        assertNull(TrialSetup.cleanCode(""))
        assertNull(TrialSetup.cleanCode("has space"))
        assertNull(TrialSetup.cleanCode("a".repeat(33)))
        assertNull(TrialSetup.cleanCode(null))
        assertEquals("Hall", TrialSetup.cleanRoom(" Ha\u0007ll "))
        assertEquals(48, TrialSetup.cleanRoom("x".repeat(80)).length)
        assertEquals(TrialArm.GUIDED, TrialArm.fromWire("nonsense"))
        assertEquals(TrialArm.UNGUIDED, TrialArm.fromWire("unguided"))
    }

    @Test fun thermalTraceKeepsMaxAndOnlyChanges() {
        val t = ThermalTrace()
        t.onLevel(0, 0); t.onLevel(0, 250); t.onLevel(3, 500); t.onLevel(1, 900); t.onLevel(1, 1_000)
        assertEquals(3, t.maxLevel())
        assertEquals(listOf(0L to 0, 500L to 3, 900L to 1), t.changesSnapshot())
    }

    @Test fun intentCounterCounts() {
        val c = IntentCounter()
        c.add("PAUSE"); c.add("MISSING"); c.add("PAUSE")
        assertEquals(mapOf("MISSING" to 1, "PAUSE" to 2), c.snapshot())
    }

    @Test fun ingestMapReadsCounters() {
        val s = IngestStats(); s.arFrames.addAndGet(5); s.framesIngested.addAndGet(2)
        val m = ResearchReport.ingestMap(s)
        assertEquals(5L, m["ar_frames"]); assertEquals(2L, m["frames_ingested"]); assertEquals(3L, m["candidate_stride"])
        s.detectorRuns.addAndGet(4); s.detectorDropped.addAndGet(1)
        val d = ResearchReport.ingestMap(s)
        assertEquals(4L, d["detector_runs"]); assertEquals(1L, d["detector_dropped"])
    }
}
