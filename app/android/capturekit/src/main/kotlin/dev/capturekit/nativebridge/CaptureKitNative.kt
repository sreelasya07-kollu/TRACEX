package dev.capturekit.nativebridge

import java.nio.ByteBuffer

/**
 * 1:1 declarations of the C API (core/include/capturekit/capi/capturekit.h) via JNI.
 * No logic lives here. Status codes: 0 OK, 1 STATE, 2 ARG, 3 IO, 4 POLICY, 5 FULL (see [Status]).
 */
object CaptureKitNative {
    init { System.loadLibrary("capturekit_jni") }

    external fun version(): String
    /** Returns the session handle (> 0) or a negative ck_status error. */
    external fun create(dir: String, configJson: String): Long
    external fun destroy(handle: Long)
    external fun start(handle: Long, tNs: Long): Int
    external fun pause(handle: Long, tNs: Long): Int
    external fun resume(handle: Long, tNs: Long): Int
    external fun stop(handle: Long, tNs: Long): Int
    external fun finalizeSession(handle: Long): Int
    external fun state(handle: Long): Int

    /** q is [w,x,y,z]; pose is ARKit/ARCore GL-axes world_from_camera (converted once in the core). */
    external fun ingestPose(
        handle: Long, tNs: Long, tx: Double, ty: Double, tz: Double,
        qw: Double, qx: Double, qy: Double, qz: Double, trackingState: Int, trackingReason: Int,
    ): Int
    external fun ingestGyro(handle: Long, tNs: Long, x: Double, y: Double, z: Double): Int
    external fun ingestAccel(handle: Long, tNs: Long, x: Double, y: Double, z: Double): Int
    external fun ingestIntrinsics(handle: Long, tNs: Long, w: Int, h: Int, fx: Double, fy: Double, cx: Double, cy: Double): Int
    /** [luma] must be a direct ByteBuffer; it is copied before this returns. */
    external fun ingestFrame(handle: Long, tNs: Long, frameId: Int, exposureNs: Long, luma: ByteBuffer, w: Int, h: Int, stride: Int): Int
    external fun ingestPoints(handle: Long, tNs: Long, xyz: FloatArray, nPoints: Int): Int
    /** d = center[3], normal[3], axis_u[3], extent_u, extent_v */
    external fun ingestPlane(handle: Long, tNs: Long, id: String, kind: String, d: DoubleArray): Int
    /** [boxes] = flat [cls, score, x0, y0, x1, y1] per box (cls as float), [nBoxes] boxes; 0 = ran, saw nothing. */
    external fun ingestDetections(handle: Long, tNs: Long, frameId: Int, boxes: FloatArray, nBoxes: Int): Int
    external fun ingestPlatform(handle: Long, tNs: Long, thermal: Int, lowPower: Boolean, batteryPct: Double, charging: Boolean, frameTimeMs: Double): Int
    external fun keyframeWritten(handle: Long, frameId: Int, ok: Boolean): Int
    /** Whole JSON lines, '\n' separated; empty when nothing is pending. */
    external fun pollEvents(handle: Long): String
    /** [level, keyframe_rate_scale, ml_rate_scale, resolution_tier, overlay, upload, must_pause] */
    external fun policy(handle: Long): DoubleArray
    external fun recover(dir: String): String
}

object Status {
    const val OK = 0
    const val STATE = 1
    const val ARG = 2
    const val IO = 3
    const val POLICY = 4
    const val FULL = 5
}
