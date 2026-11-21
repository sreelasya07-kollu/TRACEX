package dev.capturekit.verdict

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class VerdictTextTest {
    private val json = """
        {"headlines":{"FAIL":"Fail head","WARN":"Warn head","PASS":"Pass head"},
         "codes":{
          "POSE_GAP":{"severity":"WARN","level":"semantic","title":"Gap","text":"Tracking gap \"x\" é","action":"Rescan part","next":"proceed_with_note"},
          "MISSING_FILE":{"severity":"ERROR","level":"integrity","title":"Missing","text":"File gone","action":"Scan again","next":"rescan"}}}"""

    private fun manifest(vararg issues: String) =
        """{"bundle_id":"b","validation":{"verdict":"WARN","level":"semantic","issues":[${issues.joinToString(",")}]},"counts":{"keyframes":3}}"""

    @Test fun parsesHeadlinesAndCodesWithEscapes() {
        val v = VerdictText.parse(json)
        assertEquals("Fail head", v.headline("FAIL")); assertEquals("Pass head", v.headline("PASS"))
        val e = v.entry(VerdictText.Issue("POSE_GAP", "WARN", "m"))
        assertEquals("Gap", e.title); assertEquals("Tracking gap \"x\" é", e.text); assertEquals("proceed_with_note", e.next)
    }

    @Test fun unknownCodeGetsGenericEntryFromSeverity() {
        val v = VerdictText.parse(json)
        val w = v.entry(VerdictText.Issue("NEW_CODE", "WARN", "raw message"))
        assertEquals("raw message", w.text); assertEquals("proceed_with_note", w.next)
        assertEquals("rescan", v.entry(VerdictText.Issue("NEW_CODE", "ERROR", "")).next)
    }

    @Test fun damagedAssetFallsBackToBuiltInHeadlines() {
        for (bad in listOf("", "{", "[1,2]", "{\"headlines\":", "nonsense")) {
            val v = VerdictText.parse(bad)
            assertEquals("Scan looks good.", v.headline("PASS"))
            assertTrue(v.headline("FAIL").contains("scan the room again", ignoreCase = true))
        }
        assertEquals("Scan saved with notes.", VerdictText.FALLBACK.headline(null))
    }

    @Test fun issuesOfReadsCodesAndDescribeDedupesAndOrdersErrorsFirst() {
        val m = manifest(
            """{"code":"POSE_GAP","severity":"WARN","level":"semantic","message":"a"}""",
            """{"code":"POSE_GAP","severity":"WARN","level":"semantic","message":"b"}""",
            """{"code":"INFO_ONLY","severity":"INFO","level":"semantic","message":"i"}""",
            """{"code":"MISSING_FILE","severity":"ERROR","level":"integrity","message":"c"}""",
        )
        val issues = VerdictText.issuesOf(m)
        assertEquals(4, issues.size)
        val v = VerdictText.parse(json)
        assertEquals(listOf("Missing", "Gap"), v.describe(issues).map { it.title })
        assertEquals("rescan", v.nextStep("WARN", issues))
    }

    @Test fun nextStepFollowsVerdictAndEntries() {
        val v = VerdictText.parse(json)
        assertEquals("none", v.nextStep("PASS", emptyList()))
        assertEquals("proceed_with_note", v.nextStep("WARN", listOf(VerdictText.Issue("POSE_GAP", "WARN", ""))))
        assertEquals("rescan", v.nextStep("FAIL", emptyList()))
        assertEquals(emptyList<VerdictText.Issue>(), VerdictText.issuesOf("not json"))
    }

    @Test fun shippedAssetParsesWithEveryCodeComplete() {
        val f = File("../sample-app/src/main/assets/verdict_messages.json")
        if (!f.exists()) return   // module run from another directory
        val v = VerdictText.parse(f.readText())
        assertTrue(v.headline("FAIL").isNotEmpty())
        for (code in listOf("MISSING_FILE", "POSE_GAP", "FEW_KEYFRAMES", "PARTIAL_BUNDLE")) {
            val e = v.entry(VerdictText.Issue(code, "WARN", ""))
            assertTrue("$code title", e.title.isNotEmpty()); assertTrue("$code text", e.text.isNotEmpty()); assertTrue("$code action", e.action.isNotEmpty())
            assertTrue("$code next", e.next in setOf("rescan", "proceed_with_note", "none"))
        }
    }
}
