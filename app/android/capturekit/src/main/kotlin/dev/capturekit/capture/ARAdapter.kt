package dev.capturekit.capture

import android.util.Log
import com.google.ar.core.Frame
import com.google.ar.core.Plane
import com.google.ar.core.TrackingFailureReason
import com.google.ar.core.TrackingState

/**
 * ARCore -> core ingest. Called from the GL/update thread right after session.update(); it only
 * copies values into the C API (bounded queues, never blocks). Pose uses the SENSOR-oriented
 * Camera.getPose() (never getDisplayOrientedPose, conventions.md); the core converts the GL camera
 * axes to OpenCV once. ARCore quaternions are x,y,z,w: reordered to w,x,y,z here.
 */
class ARAdapter(
    private val session: SessionManager,
    private val clockOffsetNs: Long = 0L,
    /** ARCore session for getAllTrackables (planes sampled at 1 Hz, not only when updated). */
    private val arSession: () -> com.google.ar.core.Session? = { null },
) {
    private var lastIntrinsics: List<Float>? = null
    // Start one period in the past: `t - Long.MIN_VALUE` overflows negative, which silently disabled
    // points, planes and the log forever (the original "ar_points=0, ar_planes=0" bug).
    private var lastPointsNs = -POINTS_PERIOD_NS
    private var lastPlaneNs = -PLANE_PERIOD_NS
    private var lastLogNs = -LOG_PERIOD_NS
    private val planeIds = HashMap<Plane, String>()   // Plane equals/hashCode follow the native trackable
    private var nextPlane = 0
    private var lastFrameNs = Long.MIN_VALUE
    private var lastArSession: com.google.ar.core.Session? = null   // planeIds belong to one Session instance

    private val tsGuard = FrameTimestampGuard()

    fun onFrame(frame: Frame) {
        if (!tsGuard.accept(frame.timestamp)) {
            if (tsGuard.firstSkip()) Log.w("CK/Ingest", "skipping frames with timestamp 0 (ARCore has no real frame yet)")
            return
        }
        val t = frame.timestamp + clockOffsetNs
        val cam = frame.camera
        val (state, reason) = mapTracking(cam.trackingState, cam.trackingFailureReason)
        val p = cam.pose
        session.onPose(t, p.tx().toDouble(), p.ty().toDouble(), p.tz().toDouble(),
            p.qw().toDouble(), p.qx().toDouble(), p.qy().toDouble(), p.qz().toDouble(), state, reason)
        val stats = session.stats
        if (t == lastFrameNs) stats.arDupFrames.incrementAndGet()
        lastFrameNs = t
        stats.arFrames.incrementAndGet()
        stats.pose(cam.trackingState == TrackingState.TRACKING, cam.trackingFailureReason.name)
        if (t - lastLogNs >= LOG_PERIOD_NS) { lastLogNs = t; Log.i("CK/Ingest", stats.logLine()); stats.endInterval() }

        val k = cam.imageIntrinsics
        val d = k.imageDimensions
        val f = k.focalLength
        val c = k.principalPoint
        val sig = listOf(d[0].toFloat(), d[1].toFloat(), f[0], f[1], c[0], c[1])
        if (sig != lastIntrinsics) {
            lastIntrinsics = sig
            session.onIntrinsics(t, d[0], d[1], f[0].toDouble(), f[1].toDouble(), c[0].toDouble(), c[1].toDouble())
        }
        // Own try/catch: a frame-copy failure must not also starve points and planes.
        try { session.onFrame(frame, t) } catch (e: Throwable) {
            stats.onFrameThrows.incrementAndGet(); stats.lastError = "${e.javaClass.simpleName}: ${e.message}"
        }

        if (cam.trackingState != TrackingState.TRACKING) return
        if (t - lastPointsNs >= POINTS_PERIOD_NS) {
            lastPointsNs = t
            frame.acquirePointCloud().use { pc ->
                val b = pc.points
                val n = b.remaining() / 4
                if (n > 0) {
                    val xyz = FloatArray(n * 3)
                    for (i in 0 until n) { xyz[3 * i] = b.get(); xyz[3 * i + 1] = b.get(); xyz[3 * i + 2] = b.get(); b.get() }
                    session.onPoints(t, xyz, n)
                    stats.pointsIngested.addAndGet(n.toLong())
                }
            }
        }
        if (t - lastPlaneNs >= PLANE_PERIOD_NS) {
            lastPlaneNs = t
            val ars = arSession()
            if (ars !== lastArSession) { planeIds.clear(); lastArSession = ars }   // never hold Planes of a dead Session
            val planes = ars?.getAllTrackables(Plane::class.java) ?: frame.getUpdatedTrackables(Plane::class.java)
            // Only the full set (getAllTrackables) may prune ids; the updated-only fallback is partial.
            if (ars != null) planeIds.keys.retainAll(planes.toSet())
            for (plane in planes) {
                if (plane.trackingState != TrackingState.TRACKING || plane.subsumedBy != null) continue
                val pid = planeIds.getOrPut(plane) { "p${nextPlane++}" }
                val cp = plane.centerPose
                val n = cp.yAxis
                val u = cp.xAxis
                session.onPlane(t, pid, planeKind(plane.type),
                    doubleArrayOf(cp.tx().toDouble(), cp.ty().toDouble(), cp.tz().toDouble(),
                        n[0].toDouble(), n[1].toDouble(), n[2].toDouble(),
                        u[0].toDouble(), u[1].toDouble(), u[2].toDouble(),
                        plane.extentX.toDouble(), plane.extentZ.toDouble()))
                stats.planesIngested.incrementAndGet()
            }
        }
    }

    companion object {
        private const val POINTS_PERIOD_NS = 200_000_000L
        private const val PLANE_PERIOD_NS = 1_000_000_000L
        private const val LOG_PERIOD_NS = 2_000_000_000L

        /** conventions.md: tracking_state 1 NORMAL, 2 LIMITED, 3 NOT_AVAILABLE; reasons per ADR/conventions. */
        fun mapTracking(s: TrackingState, r: TrackingFailureReason): Pair<Int, Int> {
            val state = when (s) { TrackingState.TRACKING -> 1; TrackingState.PAUSED -> 2; TrackingState.STOPPED -> 3 }
            val reason = when (r) {
                TrackingFailureReason.NONE -> 0
                TrackingFailureReason.INSUFFICIENT_FEATURES -> 3
                TrackingFailureReason.INSUFFICIENT_LIGHT -> 4
                TrackingFailureReason.EXCESSIVE_MOTION -> 2
                TrackingFailureReason.BAD_STATE -> 6
                TrackingFailureReason.CAMERA_UNAVAILABLE -> 7
                else -> 8
            }
            return state to reason
        }

        /** Geometry-based classification refines this in Phase 9; ARCore gives orientation only. */
        fun planeKind(t: Plane.Type) = when (t) {
            Plane.Type.HORIZONTAL_UPWARD_FACING -> "floor"
            Plane.Type.HORIZONTAL_DOWNWARD_FACING -> "ceiling"
            else -> "wall"
        }
    }
}
