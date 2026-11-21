package dev.capturekit.segment

import android.util.Log
import com.google.ar.core.Camera
import com.google.ar.core.Frame
import com.google.ar.core.Plane
import com.google.ar.core.Session
import com.google.ar.core.TrackingState
import dev.capturekit.capture.CoreEvent

/**
 * AR frame -> segmentation job (issue #8). Call [onArFrame] on the GL thread every frame while scanning: it costs one
 * rate check unless a run is due; then it snapshots the camera, numbers the ARCore planes, builds the prompts
 * ([PromptBuilder]) from the latest core [overlay] and copies the CPU image (plane bytes only; YUV->RGB happens on the
 * worker). Never throws, never blocks: a busy worker means a counted drop ([SegmentationScheduler]).
 */
class SegmentationFeed(private val scheduler: SegmentationScheduler, private val maxPrompts: Int = 8) {
    /** Latest core overlay (objects with the detector on; layout walls). Written on the core event thread. */
    @Volatile var overlay: CoreEvent.Overlay? = null

    private val planeIds = HashMap<Plane, Long>()   // GL thread only; Plane equals/hashCode follow the native trackable
    private var nextPlane = 1L
    private var lastSession: Session? = null
    private var errors = 0L

    fun onArFrame(frame: Frame, session: Session?, mlRateScale: Double) {
        try {
            val t = frame.timestamp
            if (t == 0L) return
            val cam = frame.camera
            if (!scheduler.shouldCapture(t, mlRateScale, cam.trackingState == TrackingState.TRACKING)) return
            val snap = snapshot(cam)
            val o = overlay
            val prompts = PromptBuilder.build(snap, planes(frame, session), o?.objects ?: emptyList(),
                if (o != null && o.layoutV3) o.surfaces else emptyList(), maxPrompts)
            val image = copyImage(frame) ?: return
            scheduler.submit(SegJob(t, image, snap, prompts))
        } catch (t: Throwable) {
            if (errors++ % 50 == 0L) Log.w(TAG, "segmentation frame skipped ($errors)", t)
        }
    }

    private fun planes(frame: Frame, session: Session?): List<PlaneInfo> {
        if (session !== lastSession) { planeIds.clear(); lastSession = session }   // never hold Planes of a dead Session
        val all = session?.getAllTrackables(Plane::class.java) ?: frame.getUpdatedTrackables(Plane::class.java)
        if (session != null) planeIds.keys.retainAll(all.toSet())
        val out = ArrayList<PlaneInfo>()
        for (p in all) {
            if (p.trackingState != TrackingState.TRACKING || p.subsumedBy != null) continue
            val kind = when (p.type) {
                Plane.Type.HORIZONTAL_UPWARD_FACING -> SurfaceKind.FLOOR
                Plane.Type.HORIZONTAL_DOWNWARD_FACING -> SurfaceKind.CEILING
                else -> SurfaceKind.WALL
            }
            val c = p.centerPose
            out.add(PlaneInfo(planeIds.getOrPut(p) { nextPlane++ }, kind, Vec3(c.tx(), c.ty(), c.tz())))
        }
        return out
    }

    private fun copyImage(frame: Frame): SegImage? {
        val img = try { frame.acquireCameraImage() } catch (_: Exception) { return null }
        img.use {
            val yp = it.planes[0]; val up = it.planes[1]; val vp = it.planes[2]
            fun bytes(b: java.nio.ByteBuffer) = ByteArray(b.remaining()).also { a -> b.duplicate().get(a) }
            return SegImage(it.width, it.height, bytes(yp.buffer), yp.rowStride, bytes(up.buffer), bytes(vp.buffer), up.rowStride, up.pixelStride)
        }
    }

    companion object {
        private const val TAG = "CK/Segment"

        /** The camera of one frame: CPU-image intrinsics + physical camera pose (aligned with the CPU image). */
        fun snapshot(cam: Camera): CameraSnapshot {
            val p = cam.pose
            val k = cam.imageIntrinsics
            val f = k.focalLength; val c = k.principalPoint; val d = k.imageDimensions
            return CameraSnapshot(p.qw().toDouble(), p.qx().toDouble(), p.qy().toDouble(), p.qz().toDouble(),
                p.tx().toDouble(), p.ty().toDouble(), p.tz().toDouble(),
                f[0].toDouble(), f[1].toDouble(), c[0].toDouble(), c[1].toDouble(), d[0], d[1])
        }
    }
}
