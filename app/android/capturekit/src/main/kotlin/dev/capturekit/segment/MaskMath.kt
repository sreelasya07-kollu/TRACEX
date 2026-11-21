package dev.capturekit.segment

import dev.capturekit.capture.ObjectBox
import dev.capturekit.capture.WallSeg
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

/**
 * The AR camera at one frame: T_world_from_camera (rotation [qw]..[qz], translation [tx]..[tz]; ARCore camera axes,
 * X right / Y up / looking down -Z, aligned with the CPU image) and the CPU-image intrinsics of a [w] x [h] image.
 * The ARCore -> OpenCV axis flip happens only in [project] and [MaskWarp] (AGENTS.md C).
 */
data class CameraSnapshot(
    val qw: Double, val qx: Double, val qy: Double, val qz: Double,
    val tx: Double, val ty: Double, val tz: Double,
    val fx: Double, val fy: Double, val cx: Double, val cy: Double,
    val w: Int, val h: Int,
) {
    /** Row-major 3x3 rotation world_from_camera. */
    fun rotation(): DoubleArray {
        val n = Math.sqrt(qw * qw + qx * qx + qy * qy + qz * qz).takeIf { it > 0 } ?: 1.0
        val w = qw / n; val x = qx / n; val y = qy / n; val z = qz / n
        return doubleArrayOf(
            1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
            2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y),
        )
    }

    /** Image pixel (u, v) of world point [p], or null when it is behind or too close to the camera (< [MIN_DEPTH_M]). */
    fun project(p: Vec3): FloatArray? {
        val r = rotation()
        val dx = p.x - tx; val dy = p.y - ty; val dz = p.z - tz
        // camera (ARCore) = R^T d; OpenCV = (x, -y, -z)
        val gx = r[0] * dx + r[3] * dy + r[6] * dz
        val gy = r[1] * dx + r[4] * dy + r[7] * dz
        val gz = r[2] * dx + r[5] * dy + r[8] * dz
        val z = -gz
        if (z < MIN_DEPTH_M) return null
        return floatArrayOf((fx * gx / z + cx).toFloat(), (fy * -gy / z + cy).toFloat())
    }

    fun inImage(u: Float, v: Float, marginFrac: Float = 0.02f): Boolean {
        val mx = w * marginFrac; val my = h * marginFrac
        return u >= mx && u < w - mx && v >= my && v < h - my
    }

    companion object { const val MIN_DEPTH_M = 0.1 }
}

/**
 * Re-uses the last masks between runs (issue #8: runs are <= 2 Hz): the masks are warped by the camera ROTATION since
 * their run (infinite-depth homography H = K_to F R_to^T R_from F K_from^-1, F = ARCore->OpenCV flip). Translation is
 * ignored, so near objects lag a little while walking; the next run corrects it.
 */
object MaskWarp {
    /** Row-major 3x3 homography from [from] image pixels to [to] image pixels. */
    fun homography(from: CameraSnapshot, to: CameraSnapshot): DoubleArray {
        val kInv = doubleArrayOf(1 / from.fx, 0.0, -from.cx / from.fx, 0.0, 1 / from.fy, -from.cy / from.fy, 0.0, 0.0, 1.0)
        val k = doubleArrayOf(to.fx, 0.0, to.cx, 0.0, to.fy, to.cy, 0.0, 0.0, 1.0)
        val f = doubleArrayOf(1.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, -1.0)
        val rel = mul(transpose(to.rotation()), from.rotation())   // camera_to <- camera_from (ARCore axes)
        return mul(k, mul(f, mul(rel, mul(f, kInv))))
    }

    /** Applies [h] to (x, y) into [out] at [off]; false when the point maps behind the camera. */
    fun apply(h: DoubleArray, x: Float, y: Float, out: FloatArray, off: Int = 0): Boolean {
        val w = h[6] * x + h[7] * y + h[8]
        if (w <= 1e-9) return false
        out[off] = ((h[0] * x + h[1] * y + h[2]) / w).toFloat()
        out[off + 1] = ((h[3] * x + h[4] * y + h[5]) / w).toFloat()
        return true
    }

    private fun transpose(a: DoubleArray) = doubleArrayOf(a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8])
    private fun mul(a: DoubleArray, b: DoubleArray) = DoubleArray(9) { i ->
        val r = i / 3; val c = i % 3
        a[3 * r] * b[c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c]
    }
}

/**
 * Host-side SAM 2.1 point-prompt encoding for the litert-community mask decoder (its model card): with the bundled
 * constants posmat [2,128], point_embed[1] [256], not_a_point [256] (768 little-endian float32), a positive point
 * (x, y) in [side] x [side] model space becomes sparse_prompt [1,2,256] = [sin/cos(2 pi c @ posmat) + point_embed, not_a_point].
 */
class SamPromptEncoder(private val posmat: FloatArray, private val pointEmbed: FloatArray, private val notAPoint: FloatArray) {
    init { require(posmat.size == 2 * HALF && pointEmbed.size == DIM && notAPoint.size == DIM) }

    fun encode(x: Float, y: Float, side: Int = 1024): FloatArray {
        val out = FloatArray(2 * DIM)
        val c0 = 2.0 * (x + 0.5) / side - 1.0
        val c1 = 2.0 * (y + 0.5) / side - 1.0
        for (j in 0 until HALF) {
            val a = 2.0 * PI * (c0 * posmat[j] + c1 * posmat[HALF + j])
            out[j] = (sin(a) + pointEmbed[j]).toFloat()
            out[HALF + j] = (cos(a) + pointEmbed[HALF + j]).toFloat()
        }
        System.arraycopy(notAPoint, 0, out, DIM, DIM)
        return out
    }

    companion object {
        const val DIM = 256
        const val HALF = 128

        /** The model repo's prompt_encode_const.bin (3072 bytes), or null for any other size. */
        fun fromLittleEndian(bytes: ByteArray): SamPromptEncoder? {
            if (bytes.size != 3 * DIM * 4) return null
            val f = FloatArray(3 * DIM)
            ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(f)
            return SamPromptEncoder(f.copyOfRange(0, DIM), f.copyOfRange(DIM, 2 * DIM), f.copyOfRange(2 * DIM, 3 * DIM))
        }
    }
}

/** An ARCore plane reduced to what the prompts need: host id, kind and centre (world metres). */
data class PlaneInfo(val id: Long, val kind: SurfaceKind, val centre: Vec3)

/**
 * Builds one run's prompts (issue #8; no taps): core objects (overlay objects, only present with the detector on)
 * projected to a point (box centre) and a box; floor / ceiling from ARCore planes; walls from the core layout walls when
 * the core has them (overlay v3), else from vertical ARCore planes. Objects first, then floor, walls, ceiling; at most
 * [max] prompts, only points inside the image and in front of the camera.
 */
object PromptBuilder {
    /** Wall prompts sit this high above the floor (guess: clear of most furniture). */
    const val WALL_PROMPT_HEIGHT_M = 1.4f

    fun build(cam: CameraSnapshot, planes: List<PlaneInfo>, objects: List<ObjectBox>, layoutWalls: List<WallSeg>, max: Int = 8): List<SegPrompt> {
        val out = ArrayList<SegPrompt>()
        val floorY = planes.filter { it.kind == SurfaceKind.FLOOR }.minOfOrNull { it.centre.y }
        if (floorY != null) for (o in objects) {
            val c = Vec3(o.x.toFloat(), floorY + o.h.toFloat() / 2f, o.z.toFloat())
            val uv = cam.project(c) ?: continue
            if (!cam.inImage(uv[0], uv[1])) continue
            out.add(SegPrompt(TrackKey.CoreObject(o.id), SurfaceKind.OBJECT, o.cls, uv[0], uv[1], objectBox(cam, o, floorY), c))
        }
        fun plane(kind: SurfaceKind, label: String) {
            for (p in planes) {
                if (p.kind != kind) continue
                val uv = cam.project(p.centre) ?: continue
                if (cam.inImage(uv[0], uv[1])) out.add(SegPrompt(TrackKey.Plane(p.id), kind, label, uv[0], uv[1], null, p.centre))
            }
        }
        plane(SurfaceKind.FLOOR, "Floor")
        val walls = layoutWalls.filter { it.id > 0 }
        if (walls.isNotEmpty() && floorY != null) {
            for (s in walls) {
                val c = Vec3(((s.x0 + s.x1) / 2).toFloat(), floorY + WALL_PROMPT_HEIGHT_M, ((s.z0 + s.z1) / 2).toFloat())
                val uv = cam.project(c) ?: continue
                if (cam.inImage(uv[0], uv[1])) out.add(SegPrompt(TrackKey.LayoutWall(s.id), SurfaceKind.WALL, "Wall", uv[0], uv[1], null, c))
            }
        } else plane(SurfaceKind.WALL, "Wall")
        plane(SurfaceKind.CEILING, "Ceiling")
        return if (out.size > max) out.subList(0, max).toList() else out
    }

    /** Image box of the object's 3D box (footprint w x d rotated by yaw, floor to h), clipped to the image; null when < 2 corners project. */
    fun objectBox(cam: CameraSnapshot, o: ObjectBox, floorY: Float): Box2? {
        val a = Math.toRadians(o.yawDeg.toDouble())
        val ca = cos(a).toFloat(); val sa = sin(a).toFloat()
        var x0 = Float.MAX_VALUE; var y0 = Float.MAX_VALUE; var x1 = -Float.MAX_VALUE; var y1 = -Float.MAX_VALUE
        var n = 0
        for (sx in floatArrayOf(-0.5f, 0.5f)) for (sz in floatArrayOf(-0.5f, 0.5f)) for (yy in floatArrayOf(0f, o.h.toFloat())) {
            val lx = sx * o.w.toFloat(); val lz = sz * o.d.toFloat()
            val p = Vec3(o.x.toFloat() + lx * ca - lz * sa, floorY + yy, o.z.toFloat() + lx * sa + lz * ca)
            val uv = cam.project(p) ?: continue
            n++
            x0 = minOf(x0, uv[0]); y0 = minOf(y0, uv[1]); x1 = maxOf(x1, uv[0]); y1 = maxOf(y1, uv[1])
        }
        if (n < 2) return null
        val b = Box2(x0.coerceIn(0f, cam.w.toFloat()), y0.coerceIn(0f, cam.h.toFloat()), x1.coerceIn(0f, cam.w.toFloat()), y1.coerceIn(0f, cam.h.toFloat()))
        return if (b.area > 0f) b else null
    }
}

/** Paints one run's masks into one translucent ARGB raster (the size of the first mask); objects over surfaces. */
object MaskComposer {
    /** [alpha] 0..255 applied to every track colour. Returns null when there is nothing to draw. */
    fun argb(masks: List<SegMask>, alpha: Int): Triple<Int, Int, IntArray>? {
        if (masks.isEmpty()) return null
        val w = masks[0].mask.w; val h = masks[0].mask.h
        if (w <= 0 || h <= 0) return null
        val px = IntArray(w * h)
        val a = (alpha.coerceIn(0, 255)) shl 24
        for (m in masks.sortedBy { if (it.track.kind == SurfaceKind.OBJECT) 1 else 0 }) {
            val c = a or (m.track.color and 0xFFFFFF)
            val mb = m.mask
            if (mb.w <= 0 || mb.h <= 0 || mb.bits.size < mb.w * mb.h) continue
            for (yy in 0 until h) {
                val sy = yy * mb.h / h
                for (xx in 0 until w) if (mb.bits[sy * mb.w + xx * mb.w / w].toInt() != 0) px[yy * w + xx] = c
            }
        }
        return Triple(w, h, px)
    }
}
