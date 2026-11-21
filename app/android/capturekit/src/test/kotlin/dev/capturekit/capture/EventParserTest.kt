package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class EventParserTest {
    @Test
    fun keyframeRequest() {
        val e = EventParser.parseLine("""{"file":"keyframes/000012.jpg","frame_id":42,"t_ns":123456,"type":"keyframe_request"}""")
        assertEquals(CoreEvent.KeyframeRequest(42, "keyframes/000012.jpg"), e)
    }

    @Test
    fun multiLineAndUnknown() {
        val text = """{"type":"state","t_ns":1,"from":"IDLE","to":"CALIBRATING","reason":"start"}
{"type":"governor","t_ns":2,"from":"NOMINAL","to":"LOWER_RESOLUTION","reason":"thermal=2"}
{"type":"mystery","x":1}
not json
"""
        val ev = EventParser.parse(text)
        assertEquals(3, ev.size)
        assertEquals(CoreEvent.StateChange("IDLE", "CALIBRATING", "start"), ev[0])
        assertEquals(CoreEvent.Governor("NOMINAL", "LOWER_RESOLUTION", "thermal=2"), ev[1])
        assertTrue(ev[2] is CoreEvent.Other)
    }

    @Test
    fun frameQualityAndDrops() {
        val q = EventParser.parseLine("""{"type":"frame_quality","t_ns":5,"frame_id":7,"quality":0.0,"hard_reject":true,"reasons":["MOTION_BLUR","LOW_TEXTURE"]}""")
            as CoreEvent.FrameQuality
        assertEquals(7, q.frameId); assertTrue(q.hardReject); assertEquals(listOf("MOTION_BLUR", "LOW_TEXTURE"), q.reasons)
        val d = EventParser.parseLine("""{"type":"ingest_drop","t_ns":9,"frame":3,"gyro":10}""") as CoreEvent.IngestDrop
        assertEquals(mapOf("gyro" to 10L, "frame" to 3L), d.counts)
        assertNull(EventParser.parseLine("""{"no_type":1}"""))
    }

    @Test
    fun instructionEvent() {
        val e = EventParser.parseLine(
            """{"haptic":"tap","id":7,"instruction":"MOVE_TO","long_text":"Walk ahead about 2 meters.","msg_id":"move.to","overlay_hint":"arrow:10:2","params":{"distance_m":2,"rel_bearing_deg":10},"priority":2,"speak_text":"Walk ahead, 2 meters.","t_ns":5,"type":"instruction"}""",
        ) as CoreEvent.Instruction
        assertEquals(7, e.id); assertEquals("MOVE_TO", e.type); assertEquals("Walk ahead, 2 meters.", e.speakText)
        assertEquals(2, e.priority); assertEquals("tap", e.haptic); assertEquals("arrow:10:2", e.overlayHint)
    }

    @Test
    fun overlayEvent() {
        val e = EventParser.parseLine(
            """{"ceiling_found":false,"completeness":0.35,"floor_found":true,"has_target":true,"mode":"target","seen":0.8627,"surfaces":"1.25,4.00,3.65,4.00,86;-0.5,0.00,-0.5,3.20,12","t_ns":4999999950,"target_x":2.55,"target_z":3.05,"tracking":1,"type":"overlay","user_x":2.47,"user_yaw_deg":-2,"user_z":3.05,"wall_found":true,"walls":2,"why_id":"why.left","why_text":"Left wall not scanned yet"}""",
        ) as CoreEvent.Overlay
        assertEquals(0.35, e.completeness, 1e-9); assertEquals(0.8627, e.seen, 1e-9)
        assertEquals(1, e.tracking); assertEquals("target", e.mode)
        assertTrue(e.wallFound && e.floorFound && !e.ceilingFound); assertEquals(2, e.walls)
        assertEquals(2.47, e.userX, 1e-9); assertEquals(3.05, e.userZ, 1e-9); assertEquals(-2.0, e.userYawDeg, 1e-9)
        assertTrue(e.hasTarget); assertEquals(2.55, e.targetX, 1e-9); assertEquals(3.05, e.targetZ, 1e-9)
        assertEquals("why.left", e.whyId); assertEquals("Left wall not scanned yet", e.whyText)
        assertEquals(listOf(WallSeg(1.25, 4.0, 3.65, 4.0, 86), WallSeg(-0.5, 0.0, -0.5, 3.2, 12)), e.surfaces)
    }

    @Test
    fun overlayEmptySurfacesAndMalformed() {
        val e = EventParser.parseLine(
            """{"ceiling_found":false,"completeness":0,"floor_found":false,"has_target":false,"mode":"sweep","seen":0,"surfaces":"","t_ns":0,"target_x":0,"target_z":0,"tracking":1,"type":"overlay","user_x":2.5,"user_yaw_deg":0,"user_z":2,"wall_found":false,"walls":0,"why_id":"why.sweep","why_text":"Looking for walls: sweep slowly"}""",
        ) as CoreEvent.Overlay
        assertTrue(e.surfaces.isEmpty()); assertTrue(!e.hasTarget); assertEquals("sweep", e.mode)
        assertEquals(listOf(WallSeg(1.0, 2.0, 3.0, 4.0, 5)), EventParser.parseSurfaces("1,2,3,4,5;bad;1,2,3;x,y,z,w,v;"))
    }

    @Test
    fun overlayV2SixFieldSurfacesAndYawMask() {
        val e = EventParser.parseLine(
            """{"ceiling_found":false,"completeness":0,"floor_found":false,"has_target":false,"mode":"sweep","seen":0,"surfaces":"1.25,4.00,3.65,4.00,86,2;-0.5,0.00,-0.5,3.20,12,1","t_ns":0,"target_x":0,"target_z":0,"tracking":1,"type":"overlay","user_x":2.5,"user_yaw_deg":0,"user_z":2,"wall_found":true,"walls":2,"why_id":"why.sweep","why_text":"x","yaw_mask":2049}""",
        ) as CoreEvent.Overlay
        assertEquals(listOf(WallSeg(1.25, 4.0, 3.65, 4.0, 86, 2), WallSeg(-0.5, 0.0, -0.5, 3.2, 12, 1)), e.surfaces)
        assertEquals(2049, e.yawMask)
        // Mixed 5- and 6-field entries and a 7-field entry: 5 -> id 0, 6 -> id kept, 7 skipped.
        assertEquals(
            listOf(WallSeg(1.0, 2.0, 3.0, 4.0, 5, 0), WallSeg(1.0, 2.0, 3.0, 4.0, 5, 7)),
            EventParser.parseSurfaces("1,2,3,4,5;1,2,3,4,5,7;1,2,3,4,5,7,9"),
        )
        // Old core: no yaw_mask key -> 0, and 5-field surfaces still parse (v1 back-compat).
        val old = EventParser.parseLine(
            """{"completeness":0,"mode":"sweep","surfaces":"1,2,3,4,5","tracking":1,"type":"overlay","why_id":"a","why_text":"b"}""",
        ) as CoreEvent.Overlay
        assertEquals(0, old.yawMask)
        assertEquals(listOf(WallSeg(1.0, 2.0, 3.0, 4.0, 5)), old.surfaces)
        // Out-of-range mask bits are dropped.
        val big = EventParser.parseLine("""{"type":"overlay","surfaces":"","yaw_mask":65535}""") as CoreEvent.Overlay
        assertEquals(0xFFF, big.yawMask)
    }

    @Test
    fun policyDecode() {
        val p = Policy.from(doubleArrayOf(3.0, 0.5, 0.25, 1.0, 0.0, 0.0, 0.0))
        assertEquals("LOWER_RESOLUTION", p.levelName)
        assertTrue(!p.overlay && !p.upload && !p.mustPause)
        assertTrue(Policy.from(doubleArrayOf(5.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0)).mustPause)
    }

    @Test
    fun overlayV3SpansShellCeilingSeen() {
        val e = EventParser.parseLine(
            """{"ceiling_found":false,"ceiling_seen":true,"completeness":0.5,"floor_found":true,"has_target":false,"mode":"sweep","seen":0.5,"shell":"0,0;4,0;4,3;0,3","shell_closed":true,"spans":"1,0,0,2,0,o;1,2,0,3,0,b;1,3,0,4,0,m;-1,4,0,4,3,o","surfaces":"0,0,4,0,60,1;4,0,4,3,80,2","t_ns":1,"target_x":0,"target_z":0,"tracking":1,"type":"overlay","user_x":1,"user_yaw_deg":0,"user_z":1,"wall_found":true,"walls":4,"why_id":"w","why_text":"x"}""",
        ) as CoreEvent.Overlay
        assertTrue(e.layoutV3); assertTrue(e.shellClosed); assertTrue(e.ceilingSeen); assertEquals(4, e.walls)
        assertEquals(listOf(PlanPt(0.0, 0.0), PlanPt(4.0, 0.0), PlanPt(4.0, 3.0), PlanPt(0.0, 3.0)), e.shell)
        assertEquals(4, e.spans.size)
        assertEquals(SpanSeg(1, 2.0, 0.0, 3.0, 0.0, 'b'), e.spans[1])
        assertEquals(-1, e.spans[3].id)
        assertEquals(listOf('o', 'b', 'm', 'o'), e.spans.map { it.state })
    }

    @Test
    fun overlayWithoutV3KeysKeepsDefaults() {
        val e = EventParser.parseLine(
            """{"ceiling_found":false,"completeness":0,"floor_found":false,"has_target":false,"mode":"sweep","seen":0,"surfaces":"","t_ns":0,"target_x":0,"target_z":0,"tracking":1,"type":"overlay","user_x":0,"user_yaw_deg":0,"user_z":0,"wall_found":false,"walls":0,"why_id":"w","why_text":"x"}""",
        ) as CoreEvent.Overlay
        assertTrue(!e.layoutV3 && !e.shellClosed && !e.ceilingSeen)
        assertTrue(e.spans.isEmpty() && e.shell.isEmpty() && e.objects.isEmpty() && e.openings.isEmpty() && e.gaps.isEmpty())
    }

    @Test
    fun overlayObjectsOpeningsGaps() {
        val e = EventParser.parseLine(
            """{"ceiling_found":false,"completeness":0,"floor_found":false,"gap_why_id":"gap.occluded","gap_why_text":"behind the sofa","gaps":"occluded,1.5,2.0,0.8;missing_wall,0,4,0.5","has_target":false,"mode":"sweep","objects":"1,sofa,1.2,0.4,1.8,0.9,15,0.8;2,chair,3,1,0.5,0.5,0,0.9","openings":"door,W1,0,0,0.9,0,0,2;window,,4,1,4,2,0.9,1.9","seen":0,"surfaces":"","t_ns":0,"target_x":0,"target_z":0,"tracking":1,"type":"overlay","user_x":0,"user_yaw_deg":0,"user_z":0,"wall_found":false,"walls":0,"why_id":"w","why_text":"x"}""",
        ) as CoreEvent.Overlay
        assertEquals(ObjectBox(1, "sofa", 1.2, 0.4, 1.8, 0.9, 15, 0.8), e.objects[0]); assertEquals(2, e.objects.size)
        assertEquals(Opening("door", "W1", 0.0, 0.0, 0.9, 0.0, 0.0, 2.0), e.openings[0]); assertEquals("", e.openings[1].wall)
        assertEquals(GapMark("occluded", 1.5, 2.0, 0.8), e.gaps[0]); assertEquals("behind the sofa", e.gapWhyText)
    }

    @Test
    fun reviewHandOverEncodersRoundTrip() {
        val shell = listOf(PlanPt(0.0, 0.0), PlanPt(4.5, 0.0), PlanPt(4.5, -3.25))
        assertEquals(shell, EventParser.parseShell(EventParser.encodeShell(shell)))
        val spans = listOf(SpanSeg(2, 0.0, 0.0, 1.5, 0.0, 'b'), SpanSeg(-1, 1.5, 0.0, 4.5, 0.0, 'm'))
        assertEquals(spans, EventParser.parseSpans(EventParser.encodeSpans(spans)))
        val objs = listOf(ObjectBox(3, "sofa", 1.0, 2.0, 1.8, 0.9, 30, 0.8))
        assertEquals(objs, EventParser.parseObjects(EventParser.encodeObjects(objs)))
        val ops = listOf(Opening("door", "W1", 0.0, 0.0, 0.9, 0.0, 0.0, 2.0), Opening("window", "", 4.0, 1.0, 4.0, 2.0, 0.9, 1.9))
        assertEquals(ops, EventParser.parseOpenings(EventParser.encodeOpenings(ops)))
        assertEquals(emptyList<PlanPt>(), EventParser.parseShell(EventParser.encodeShell(emptyList())))
    }

    @Test
    fun malformedV3RowsAreSkipped() {
        assertTrue(EventParser.parseSpans("").isEmpty())
        assertEquals(1, EventParser.parseSpans("1,0,0,1,0,o;1,0,0,1,0,x;a,0,0,1,0,o;1,0,0,1,o;").size)
        assertEquals(1, EventParser.parseShell("1,2;x,3;4").size)
        assertEquals(1, EventParser.parseObjects("1,sofa,0,0,1,1,0,1;1,sofa,0,0,1,1,0;1,sofa,q,0,1,1,0,1").size)
        assertEquals(0, EventParser.parseOpenings("door,W1,0,0,1,0,0").size)
        assertEquals(0, EventParser.parseGaps("occluded,1,two,0.5").size)
    }

    @Test
    fun trackingRecoveryEvents() {
        // Exactly as the core writes them (sorted keys); t_ns keeps all 64 bits.
        val lost = EventParser.parseLine("""{"degraded_s":2.013,"reason":3,"t_ns":9007199254740993,"tracking_state":2,"type":"tracking_lost"}""")
        assertEquals(CoreEvent.TrackingLost(9007199254740993L, 2, 3, 2.013), lost)
        val reset = EventParser.parseLine("""{"drift_deg":4.5,"drift_m":0.123,"lost_s":3.5,"t_ns":12000000000,"type":"tracking_reset"}""")
        assertEquals(CoreEvent.TrackingReset(12_000_000_000L, 3.5, 0.123, 4.5), reset)
        assertNull(EventParser.parseLine("""{"type":"tracking_lost","reason":3}"""))   // no t_ns: malformed, skipped
    }

    @Test
    fun configJsonEscapes() {
        assertEquals("""{"a":1,"b":"x\"y","c":true}""", SessionManager.toJson(linkedMapOf("a" to 1, "b" to "x\"y", "c" to true)))
    }
}
