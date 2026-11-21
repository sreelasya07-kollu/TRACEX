package dev.capturekit.guidance

import dev.capturekit.capture.EventParser
import dev.capturekit.capture.CoreEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class StateAnswersTest {
    private fun snap(
        completeness: Double = 0.4, tracking: Int = 1, wall: Boolean = true, floor: Boolean = true,
        ceiling: Boolean = true, walls: List<WallState> = emptyList(), sectors: Int = 4,
        gaps: List<String> = emptyList(),
    ) = ScanStateSnapshot(completeness, tracking, wall, floor, ceiling, walls, sectors, gaps = gaps)

    @Test
    fun unknownStateAbstains() {
        assertEquals(StateAnswers.ABSTAIN, StateAnswers.answer(StateQuestion.MISSING, null).text)
        assertTrue(StateAnswers.answer(StateQuestion.MISSING, null).abstained)
        val empty = snap(0.0, 1, false, false, false, emptyList(), 0)
        assertFalse(empty.hasScanData)
        assertTrue(StateAnswers.answer(StateQuestion.MISSING, empty).abstained)
        assertTrue(StateAnswers.answer(StateQuestion.PROGRESS, empty).abstained)
        // Tracking not available: the snapshot is stale, do not describe it.
        assertTrue(StateAnswers.answer(StateQuestion.MISSING, snap(tracking = 3, walls = listOf(WallState(1, 10)))).abstained)
        // Unknown question.
        val a = StateAnswers.answer(null, snap(walls = listOf(WallState(1, 10))))
        assertTrue(a.abstained); assertEquals(StateAnswers.ABSTAIN, a.text); assertTrue(a.cites.isEmpty())
    }

    @Test
    fun missingNamesWeakWallsAndFloorFromSnapshotOnly() {
        val s = snap(
            walls = listOf(WallState(3, 95), WallState(2, 40), WallState(1, 79)),
            floor = false, ceiling = true,
        )
        val a = StateAnswers.answer(StateQuestion.MISSING, s)
        assertFalse(a.abstained)
        assertEquals("Missing: Wall 2, 40 percent; Wall 1, 79 percent; the floor.", a.text)
        assertEquals(listOf("Wall 2", "Wall 1"), a.cites)
        assertTrue(s.regions.containsAll(a.cites))
        // Wall 3 is done and not mentioned; no wall number outside the snapshot appears.
        assertFalse(a.text.contains("Wall 3"))
        val named = Regex("Wall (\\d+)").findAll(a.text).map { it.groupValues[1].toInt() }.toSet()
        assertTrue(s.walls.map { it.id }.containsAll(named))
    }

    @Test
    fun missingCapsListAndCitesOnlySpoken() {
        val s = snap(
            walls = listOf(WallState(1, 10), WallState(2, 20), WallState(3, 30), WallState(4, 40)),
            floor = false, ceiling = false,
        )
        val a = StateAnswers.answer(StateQuestion.MISSING, s)
        assertEquals("Missing: Wall 1, 10 percent; Wall 2, 20 percent; Wall 3, 30 percent; and 2 more.", a.text)
        assertEquals(listOf("Wall 1", "Wall 2", "Wall 3"), a.cites)
    }

    @Test
    fun missingCitesGapsAsGiven() {
        val s = snap(completeness = 0.95, walls = listOf(WallState(1, 90)), gaps = listOf("Window area behind you"))
        val a = StateAnswers.answer(StateQuestion.MISSING, s)
        assertEquals("Missing: Window area behind you.", a.text)
        assertEquals(listOf("Window area behind you"), a.cites)
    }

    @Test
    fun nothingMissing() {
        val done = snap(completeness = 0.95, walls = listOf(WallState(1, 90), WallState(2, 85)))
        assertEquals("Nothing is missing. You can finish.", StateAnswers.answer(StateQuestion.MISSING, done).text)
        val early = snap(completeness = 0.3, walls = listOf(WallState(1, 90)))
        assertEquals("Nothing specific yet. Keep scanning.", StateAnswers.answer(StateQuestion.MISSING, early).text)
    }

    @Test
    fun noWallYetButSomeDataIsAnswered() {
        val a = StateAnswers.answer(StateQuestion.MISSING, snap(completeness = 0.0, wall = false, floor = true, ceiling = false, sectors = 3))
        assertEquals("Missing: no wall found yet.", a.text)
        assertTrue(a.cites.isEmpty())
    }

    @Test
    fun progressTemplate() {
        assertEquals(
            "Scan is 40 percent complete, 2 walls found.",
            StateAnswers.answer(StateQuestion.PROGRESS, snap(walls = listOf(WallState(1, 1), WallState(2, 2)))).text,
        )
        assertEquals("Scan is 40 percent complete, 1 wall found.", StateAnswers.answer(StateQuestion.PROGRESS, snap(walls = listOf(WallState(5, 1)))).text)
    }

    @Test
    fun onlyMissingIntentIsAStateQuestion() {
        assertEquals(StateQuestion.MISSING, StateAnswers.questionFor(VoiceIntent.MISSING))
        VoiceIntent.values().filter { it != VoiceIntent.MISSING }.forEach { assertNull(StateAnswers.questionFor(it)) }
    }

    @Test
    fun snapshotFromOverlayUsesWallIdsAndFallsBackToPosition() {
        val e = EventParser.parseLine(
            """{"ceiling_found":false,"completeness":0.42,"floor_found":true,"mode":"sweep","surfaces":"1.25,4.00,3.65,4.00,86,2;-0.5,0.00,-0.5,3.20,12,1","tracking":1,"type":"overlay","wall_found":true,"walls":2,"why_id":"a","why_text":"b","yaw_mask":2049}""",
        ) as CoreEvent.Overlay
        val s = ScanStateSnapshot.fromOverlay(e)
        assertEquals(listOf(WallState(2, 86), WallState(1, 12)), s.walls)
        assertEquals(2, s.headingSectors) // 2049 = 0x801 = bits 0 and 11
        assertEquals(0.42, s.completeness, 1e-9)
    }

    @Test
    fun v1CoreWallsGetPositionalNames() {
        val e = EventParser.parseLine("""{"type":"overlay","completeness":0.1,"surfaces":"0,0,1,0,50;0,0,0,1,20","tracking":1}""") as CoreEvent.Overlay
        assertEquals(listOf("Wall 1", "Wall 2"), ScanStateSnapshot.fromOverlay(e).walls.map { it.name })
    }

    @Test
    fun fallbackWallIdsNeverCollideWithRealIds() {
        val e = EventParser.parseLine("""{"type":"overlay","completeness":0.1,"surfaces":"0,0,1,0,50,2;0,0,0,1,20;0,0,0,2,30,1;0,0,0,3,40","tracking":1}""") as CoreEvent.Overlay
        val ids = ScanStateSnapshot.fromOverlay(e).walls.map { it.id }
        assertEquals(listOf(2, 3, 1, 4), ids)
        assertEquals(ids.size, ids.toSet().size)
    }


    @Test
    fun ceilingIsNeverListedAsMissing() {
        val a = StateAnswers.answer(StateQuestion.MISSING, snap(walls = listOf(WallState(1, 95)), floor = true, ceiling = false, completeness = 0.95))
        assertEquals("Nothing is missing. You can finish.", a.text)
        assertFalse(a.text.contains("ceiling"))
    }

    private fun v3(surfaces: String, spans: String, walls: Int, extra: String = "") = EventParser.parseLine(
        """{"type":"overlay","completeness":0.5,"floor_found":true,"wall_found":true,"ceiling_found":false,$extra"shell":"","shell_closed":false,"spans":"$spans","surfaces":"$surfaces","tracking":1,"walls":$walls}""",
    ) as CoreEvent.Overlay

    @Test
    fun v3SnapshotKeepsCoreIdsDropsInferredAndFullyBlockedWalls() {
        // ids 3, 1 real; id 0 = plane-less / hypothesised side; wall 2 is hidden behind furniture on every span.
        val e = v3("0,0,4,0,60,3;4,0,4,3,0,0;4,3,0,3,10,2;0,3,0,0,30,1", "3,0,0,4,0,o;2,4,3,2,3,b;2,2,3,0,3,b;1,0,3,0,0,o", walls = 3, extra = "\"ceiling_seen\":true,")
        val s = ScanStateSnapshot.fromOverlay(e)
        assertEquals(listOf(WallState(3, 60), WallState(1, 30)), s.walls)
        assertEquals(3, s.wallCount)
        assertTrue(s.ceilingFound)
        assertEquals("Scan is 50 percent complete, 3 walls found.", StateAnswers.answer(StateQuestion.PROGRESS, s).text)
        // A partly blocked wall stays.
        val e2 = v3("0,0,4,0,10,2", "2,0,0,2,0,b;2,2,0,4,0,o", walls = 1)
        assertEquals(listOf(WallState(2, 10)), ScanStateSnapshot.fromOverlay(e2).walls)
    }
    @Test
    fun nonFiniteCompletenessAndControlCharsAreSafeInJson() {
        for (bad in listOf(Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY)) {
            val e = EventParser.parseLine("""{"type":"overlay","surfaces":"","tracking":1}""") as CoreEvent.Overlay
            assertEquals(0.0, ScanStateSnapshot.fromOverlay(e.copy(completeness = bad)).completeness, 0.0)
            val raw = ScanStateSnapshot(bad, 1, true, true, true, emptyList(), 0)
            assertTrue(raw.toJson().contains("\"completeness\":0.0"))
        }
        val s = ScanStateSnapshot(0.5, 1, true, true, true, emptyList(), 0, gaps = listOf("a\nb\u0001c\td"))
        assertTrue(s.toJson().contains("\"gaps\":[\"a\\nb\\u0001c\\td\"]"))
        assertFalse(s.toJson().any { it.code < 0x20 })
    }

    @Test
    fun jsonIsSortedFlatAndHasNoImageOrPathFields() {
        val s = ScanStateSnapshot(0.4, 1, true, true, false, listOf(WallState(1, 12)), 2, labels = listOf("door"), gaps = listOf("say \"hi\""))
        assertEquals(
            """{"ceiling_found":false,"completeness":0.4,"floor_found":true,"gaps":["say \"hi\""],"heading_sectors":2,"labels":["door"],"tracking":1,"wall_found":true,"walls":[{"id":1,"pct":12}]}""",
            s.toJson(),
        )
    }
}
