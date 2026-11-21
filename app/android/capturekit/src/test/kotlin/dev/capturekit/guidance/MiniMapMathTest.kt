package dev.capturekit.guidance

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * World (x, z) -> heading-up screen metres (right = +x_s, down = +y_s, the user at the origin, forward = screen up).
 * Yaw convention: user_yaw_deg positive = left, 0 = world +z, so the heading is (sin yaw, cos yaw) in (x, z) and the
 * user's left is (cos yaw, -sin yaw).
 */
class MiniMapMathTest {
    private val out = FloatArray(2)
    private fun at(x: Double, z: Double, yaw: Double, rot: Int = 0, follows: Boolean = true, ux: Double = 0.0, uz: Double = 0.0): Pair<Float, Float> {
        MiniMapMath.toScreen(x, z, ux, uz, yaw, rot, follows, out)
        return out[0] to out[1]
    }
    private fun assertPt(ex: Float, ey: Float, p: Pair<Float, Float>) {
        assertEquals("x", ex, p.first, 1e-4f); assertEquals("y", ey, p.second, 1e-4f)
    }

    // The same expectations hold in portrait (0) and both landscapes (90, 270): the view is laid out in the display's
    // orientation and the core's yaw is the camera heading, independent of how the phone is held.
    private val rotations = listOf(0, 90, 270)

    @Test fun yawZeroForwardIsUp() = rotations.forEach { r ->
        assertPt(0f, -2f, at(0.0, 2.0, 0.0, r))      // 2 m ahead (+z) -> up
        assertPt(0f, 2f, at(0.0, -2.0, 0.0, r))      // behind -> down
        assertPt(-3f, 0f, at(3.0, 0.0, 0.0, r))      // +x is the user's LEFT at yaw 0 -> screen left
        assertPt(3f, 0f, at(-3.0, 0.0, 0.0, r))
    }

    @Test fun yawPlus90FacesPlusX() = rotations.forEach { r ->
        assertPt(0f, -2f, at(2.0, 0.0, 90.0, r))     // now +x is ahead
        assertPt(0f, 2f, at(-2.0, 0.0, 90.0, r))
        assertPt(-3f, 0f, at(0.0, -3.0, 90.0, r))    // left of +x is -z
        assertPt(3f, 0f, at(0.0, 3.0, 90.0, r))
    }

    @Test fun yawMinus90FacesMinusX() = rotations.forEach { r ->
        assertPt(0f, -2f, at(-2.0, 0.0, -90.0, r))
        assertPt(-3f, 0f, at(0.0, 3.0, -90.0, r))    // left of -x is +z
        assertPt(3f, 0f, at(0.0, -3.0, -90.0, r))
    }

    @Test fun yaw180FacesMinusZ() = rotations.forEach { r ->
        assertPt(0f, -2f, at(0.0, -2.0, 180.0, r))
        assertPt(0f, 2f, at(0.0, 2.0, 180.0, r))
        assertPt(-3f, 0f, at(-3.0, 0.0, 180.0, r))   // left of -z is -x
    }

    @Test fun userPositionIsTheOrigin() = rotations.forEach { r ->
        assertPt(0f, 0f, at(1.5, -2.5, 37.0, r, ux = 1.5, uz = -2.5))
        assertPt(0f, 1f, at(1.5, -1.5, 180.0, r, ux = 1.5, uz = -2.5))   // 1 m toward +z of a user facing -z = 1 m behind
    }

    @Test fun turningLeftRotatesTheMapClockwise() {
        // A fixed landmark straight ahead at yaw 0 ends up on the user's right after the user turns left 90 deg.
        assertPt(0f, -2f, at(0.0, 2.0, 0.0))
        assertPt(2f, 0f, at(0.0, 2.0, 90.0))
    }

    @Test fun distanceIsPreserved() {
        for (yaw in listOf(0.0, 13.0, 90.0, 123.0, -45.0, 180.0, 271.0)) {
            val p = at(3.0, 4.0, yaw)
            assertEquals(5f, kotlin.math.hypot(p.first, p.second), 1e-4f)
        }
    }

    @Test fun nonFollowingViewCounterRotatesByTheDisplayRotation() {
        // A view that stays in the natural orientation while the display is rotated 90 (clockwise content rotation):
        // forward must stay physically up, so the map is rotated back by the display rotation.
        assertPt(0f, -2f, at(0.0, 2.0, 0.0, 0, follows = false))
        val p90 = at(0.0, 2.0, 0.0, 90, follows = false)
        assertPt(-2f, 0f, p90)                        // forward points to the screen left of a natural-oriented view
        val p270 = at(0.0, 2.0, 0.0, 270, follows = false)
        assertPt(2f, 0f, p270)
    }

    @Test fun headingArrowIsAlwaysUp() {
        assertPt(0f, -1f, at(kotlin.math.sin(Math.toRadians(70.0)), kotlin.math.cos(Math.toRadians(70.0)), 70.0))
    }

    @Test fun relativeBearingMatchesSpokenLeftRight() {
        // +bearing = target to the user's left (the core's yaw sign); used for a target hint arrow.
        assertEquals(90.0, MiniMapMath.relativeBearingDeg(3.0, 0.0, 0.0, 0.0, 0.0), 1e-6)    // +x is left at yaw 0
        assertEquals(-90.0, MiniMapMath.relativeBearingDeg(-3.0, 0.0, 0.0, 0.0, 0.0), 1e-6)
        assertEquals(0.0, MiniMapMath.relativeBearingDeg(0.0, 5.0, 0.0, 0.0, 0.0), 1e-6)
        assertEquals(180.0, Math.abs(MiniMapMath.relativeBearingDeg(0.0, -5.0, 0.0, 0.0, 0.0)), 1e-6)
    }
}
