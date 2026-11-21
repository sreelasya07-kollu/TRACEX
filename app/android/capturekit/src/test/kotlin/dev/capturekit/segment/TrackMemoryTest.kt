package dev.capturekit.segment

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

class TrackMemoryTest {
    private fun chair(key: TrackKey?, centre: Vec3? = null, box: Box2? = null) =
        SegPrompt(key, SurfaceKind.OBJECT, "chair", 10f, 10f, box, centre)
    private fun floor(id: Long, centre: Vec3? = null, box: Box2? = null) =
        SegPrompt(TrackKey.Plane(id), SurfaceKind.FLOOR, "Floor", 5f, 5f, box, centre)

    @Test fun sameObjectLeavesAndReturnsKeepsIdAndColour() {
        val m = TrackMemory()
        val first = m.assign(listOf(chair(TrackKey.CoreObject(7), Vec3(1f, 0.4f, 2f)))).single()
        repeat(20) { m.assign(listOf(floor(1))) }   // the chair is out of view for 20 runs
        val back = m.assign(listOf(chair(TrackKey.CoreObject(7), Vec3(1.05f, 0.4f, 2f)))).single()
        assertEquals(first.id, back.id)
        assertEquals(first.color, back.color)
        assertEquals("chair", back.label)
    }

    @Test fun unkeyedObjectReturnsByCentreDistance() {
        val m = TrackMemory()
        val a = m.assign(listOf(chair(null, Vec3(1f, 0.4f, 2f), Box2(0f, 0f, 50f, 50f)))).single()
        repeat(5) { m.assign(listOf(floor(1))) }
        // Re-entering the view from the other side: the box is elsewhere, the 3D centre is close.
        val b = m.assign(listOf(chair(null, Vec3(1.2f, 0.4f, 2.1f), Box2(400f, 300f, 460f, 360f)))).single()
        assertEquals(a.id, b.id)
        val far = m.assign(listOf(chair(null, Vec3(4f, 0.4f, -2f), Box2(400f, 300f, 460f, 360f)))).single()
        assertNotEquals(a.id, far.id)
    }

    @Test fun iouFallbackWithoutCentre() {
        val m = TrackMemory()
        val a = m.assign(listOf(chair(null, box = Box2(100f, 100f, 200f, 200f)))).single()
        val b = m.assign(listOf(chair(null, box = Box2(110f, 105f, 210f, 205f)))).single()
        assertEquals(a.id, b.id)
        val c = m.assign(listOf(chair(null, box = Box2(400f, 400f, 450f, 450f)))).single()
        assertNotEquals(a.id, c.id)
    }

    @Test fun differentLabelOrKindNeverMerges() {
        val m = TrackMemory()
        val a = m.assign(listOf(chair(null, Vec3(1f, 0.4f, 2f), Box2(0f, 0f, 50f, 50f)))).single()
        val t = m.assign(listOf(SegPrompt(null, SurfaceKind.OBJECT, "table", 10f, 10f, Box2(0f, 0f, 50f, 50f), Vec3(1f, 0.4f, 2f)))).single()
        assertNotEquals(a.id, t.id)
    }

    @Test fun distinctKeysAreDistinctTracksEvenWhenClose() {
        val m = TrackMemory()
        val (a, b) = m.assign(listOf(chair(TrackKey.CoreObject(1), Vec3(1f, 0.4f, 2f)), chair(TrackKey.CoreObject(2), Vec3(1.1f, 0.4f, 2f))))
        assertNotEquals(a.id, b.id)
        val again = m.assign(listOf(chair(TrackKey.CoreObject(2), Vec3(1f, 0.4f, 2f)))).single()
        assertEquals(b.id, again.id)
    }

    @Test fun oneTrackPerPromptPerRun() {
        val m = TrackMemory()
        val c = Vec3(1f, 0.4f, 2f)
        val (a, b) = m.assign(listOf(chair(null, c), chair(null, c)))
        assertNotEquals(a.id, b.id)
        val (a2, b2) = m.assign(listOf(chair(null, c), chair(null, c)))
        assertEquals(setOf(a.id, b.id), setOf(a2.id, b2.id))
    }

    @Test fun keyedPromptAdoptsUnkeyedTrackThenKeepsIt() {
        val m = TrackMemory()
        val a = m.assign(listOf(chair(null, Vec3(1f, 0.4f, 2f)))).single()
        val b = m.assign(listOf(chair(TrackKey.CoreObject(5), Vec3(1.1f, 0.4f, 2f)))).single()
        assertEquals(a.id, b.id)
        val c = m.assign(listOf(chair(TrackKey.CoreObject(5), Vec3(3f, 0.4f, 5f)))).single()   // the core moved it: key wins
        assertEquals(a.id, c.id)
    }

    @Test fun lruBoundEvictsLeastRecentlySeen() {
        val m = TrackMemory(capacity = 3)
        val ids = (1..3).map { m.assign(listOf(chair(TrackKey.CoreObject(it)))).single().id }
        m.assign(listOf(chair(TrackKey.CoreObject(1))))            // touch 1: now 2 is the oldest
        m.assign(listOf(chair(TrackKey.CoreObject(4))))            // evicts 2
        assertEquals(3, m.size)
        assertEquals(ids[0], m.assign(listOf(chair(TrackKey.CoreObject(1)))).single().id)
        assertEquals(ids[2], m.assign(listOf(chair(TrackKey.CoreObject(3)))).single().id)
        val two = m.assign(listOf(chair(TrackKey.CoreObject(2)))).single().id
        assertNotEquals(ids[1], two)                               // forgotten: a new id, never a reused one
        assertEquals(3, m.size)
    }

    @Test fun planeIdStableWhileItsBoxAndCentreMove() {
        val m = TrackMemory()
        val a = m.assign(listOf(floor(42, Vec3(0f, 0f, 0f), Box2(0f, 300f, 640f, 480f)))).single()
        val b = m.assign(listOf(floor(42, Vec3(3f, 0f, 4f), Box2(0f, 0f, 100f, 50f)))).single()   // grew / re-centred
        assertEquals(a.id, b.id)
        val other = m.assign(listOf(floor(43, Vec3(3f, 0f, 4f), Box2(0f, 0f, 100f, 50f)))).single()
        assertNotEquals(a.id, other.id)
        assertEquals(SurfaceKind.FLOOR, other.kind)
        val wall = m.assign(listOf(SegPrompt(TrackKey.LayoutWall(3), SurfaceKind.WALL, "Wall", 1f, 1f))).single()
        assertEquals(wall.id, m.assign(listOf(SegPrompt(TrackKey.LayoutWall(3), SurfaceKind.WALL, "Wall", 300f, 9f))).single().id)
    }

    @Test fun colourIsAFunctionOfTheIdAndNeighboursDiffer() {
        val m = TrackMemory()
        val t = m.assign((1..4).map { chair(TrackKey.CoreObject(it)) })
        for (i in 1 until t.size) assertNotEquals(t[i - 1].color, t[i].color)
        t.forEach { assertEquals(TrackMemory.colorFor(it.id), it.color) }
    }

    @Test fun boxIou() {
        assertEquals(1f, Box2(0f, 0f, 10f, 10f).iou(Box2(0f, 0f, 10f, 10f)), 1e-6f)
        assertEquals(0f, Box2(0f, 0f, 10f, 10f).iou(Box2(20f, 20f, 30f, 30f)), 1e-6f)
        assertEquals(25f / 175f, Box2(0f, 0f, 10f, 10f).iou(Box2(5f, 5f, 15f, 15f)), 1e-6f)
    }
}
