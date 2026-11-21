package dev.capturekit.guidance

import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin

/**
 * Heading-up mini-map transform (real-room-fix T3). Pure, allocation-free, JVM-testable.
 *
 * World frame: Y-up, plan view (x, z), metres. The core's `user_yaw_deg` is positive = left and 0 = world +z, so the
 * user's heading is (sin yaw, cos yaw) in (x, z) and the user's left is (cos yaw, -sin yaw).
 * Screen frame: metres from the user, +x right, +y DOWN, forward = screen up (negative y).
 *
 * Display rotation: the core's yaw is the camera heading in the world, independent of how the phone is held, and the
 * sample's activities are laid out in the display's orientation (portrait 0, landscape 90 / 270), so the result is the
 * same for every rotation when [toScreen]'s `viewFollowsDisplay` is true (the default and the only mode the sample uses).
 * A host whose view stays in the natural orientation while the display rotates passes `viewFollowsDisplay = false`:
 * the map is then rotated by [displayRotationDeg] counter-clockwise on screen so forward stays physically up.
 */
object MiniMapMath {
    /** Writes the screen offset (metres, [out][0] = right, [out][1] = down) of world point ([x], [z]) for a user at ([userX], [userZ]) with [userYawDeg]. */
    fun toScreen(
        x: Double, z: Double, userX: Double, userZ: Double, userYawDeg: Double,
        displayRotationDeg: Int = 0, viewFollowsDisplay: Boolean = true, out: FloatArray,
    ) {
        val a = Math.toRadians(userYawDeg)
        val s = sin(a); val c = cos(a)
        val dx = x - userX; val dz = z - userZ
        val forward = dx * s + dz * c
        val left = dx * c - dz * s
        var sx = -left
        var sy = -forward
        if (!viewFollowsDisplay) {
            val t = Math.toRadians((((displayRotationDeg % 360) + 360) % 360).toDouble())
            val ct = cos(t); val st = sin(t)
            val rx = sx * ct + sy * st
            val ry = -sx * st + sy * ct
            sx = rx; sy = ry
        }
        out[0] = sx.toFloat(); out[1] = sy.toFloat()
    }

    /** Bearing of a world point relative to the user's heading in degrees, (-180, 180]; positive = to the user's left (the core's sign). */
    fun relativeBearingDeg(x: Double, z: Double, userX: Double, userZ: Double, userYawDeg: Double): Double {
        val a = Math.toRadians(userYawDeg)
        val dx = x - userX; val dz = z - userZ
        val forward = dx * sin(a) + dz * cos(a)
        val left = dx * cos(a) - dz * sin(a)
        return Math.toDegrees(atan2(left, forward))
    }
}
