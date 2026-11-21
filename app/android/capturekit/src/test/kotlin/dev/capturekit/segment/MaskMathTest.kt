package dev.capturekit.segment

import dev.capturekit.capture.ObjectBox
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.cos
import kotlin.math.sin

class MaskMathTest {
    /** Camera at the origin looking down world -Z (identity rotation), 640 x 480. */
    private val cam = CameraSnapshot(1.0, 0.0, 0.0, 0.0, 0.0, 1.5, 0.0, 500.0, 500.0, 320.0, 240.0, 640, 480)

    private fun yawed(deg: Double): CameraSnapshot {
        val h = Math.toRadians(deg) / 2
        return cam.copy(qw = cos(h), qy = sin(h))
    }

    @Test fun projectFollowsOpenCvImageAxes() {
        val c = cam.project(Vec3(0f, 1.5f, -2f))!!
        assertEquals(320f, c[0], 1e-3f); assertEquals(240f, c[1], 1e-3f)
        val rightUp = cam.project(Vec3(1f, 2.5f, -2f))!!
        assertEquals(320f + 250f, rightUp[0], 1e-3f)   // world +X = image right
        assertEquals(240f - 250f, rightUp[1], 1e-3f)   // world up = image up (smaller v)
        assertNull(cam.project(Vec3(0f, 1.5f, 2f)))    // behind
    }

    @Test fun warpIsIdentityWithoutMotion() {
        val h = MaskWarp.homography(cam, cam)
        val out = FloatArray(2)
        assertTrue(MaskWarp.apply(h, 100f, 50f, out))
        assertEquals(100f, out[0], 1e-3f); assertEquals(50f, out[1], 1e-3f)
    }

    @Test fun warpMatchesReprojectionUnderPureRotation() {
        val to = yawed(10.0)
        val h = MaskWarp.homography(cam, to)
        val out = FloatArray(2)
        for (p in listOf(Vec3(0.3f, 1.2f, -3f), Vec3(-0.5f, 2f, -1.5f), Vec3(0.1f, 1.5f, -6f))) {
            val a = cam.project(p)!!; val b = to.project(p)!!
            assertTrue(MaskWarp.apply(h, a[0], a[1], out))
            assertEquals(b[0], out[0], 0.05f); assertEquals(b[1], out[1], 0.05f)
        }
    }

    @Test fun promptEncoderLayout() {
        val bytes = ByteBuffer.allocate(3072).order(ByteOrder.LITTLE_ENDIAN)
        repeat(256) { bytes.putFloat(0f) }            // posmat = 0 -> sin 0, cos 1
        repeat(256) { bytes.putFloat(0.5f) }          // point_embed[1]
        repeat(256) { bytes.putFloat(-2f) }           // not_a_point
        val e = SamPromptEncoder.fromLittleEndian(bytes.array())!!
        val t = e.encode(512f, 512f)
        assertEquals(512, t.size)
        assertEquals(0.5f, t[0], 1e-6f); assertEquals(1.5f, t[128], 1e-6f); assertEquals(-2f, t[256], 1e-6f)
        assertNull(SamPromptEncoder.fromLittleEndian(ByteArray(100)))
    }

    @Test fun promptsKeepObjectsFirstAndOnlyVisibleOnes() {
        val planes = listOf(
            PlaneInfo(1, SurfaceKind.FLOOR, Vec3(0f, 0f, -6f)),
            PlaneInfo(2, SurfaceKind.CEILING, Vec3(0f, 2.6f, 4f)),   // behind: no prompt
            PlaneInfo(3, SurfaceKind.WALL, Vec3(0f, 1.5f, -4f)),
        )
        val chair = ObjectBox(9, "chair", 0.2, -2.5, 0.5, 0.5, 0, 0.9)
        val p = PromptBuilder.build(cam, planes, listOf(chair), emptyList())
        assertEquals(listOf("chair", "Floor", "Wall"), p.map { it.label })
        assertEquals(TrackKey.CoreObject(9), p[0].key)
        assertNotNull(p[0].box)
        assertEquals(TrackKey.Plane(3), p[2].key)
        assertEquals(2, PromptBuilder.build(cam, planes, listOf(chair), emptyList(), max = 2).size)
        // Without a floor plane there is no height for objects / layout walls.
        assertEquals(listOf("Wall"), PromptBuilder.build(cam, planes.filter { it.kind != SurfaceKind.FLOOR }, listOf(chair), emptyList()).map { it.label })
    }
}
