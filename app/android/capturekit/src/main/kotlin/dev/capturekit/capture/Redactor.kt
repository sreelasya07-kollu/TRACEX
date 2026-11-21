package dev.capturekit.capture

import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** Pixel rectangle, half-open: x0..x1 and y0..y1 exclusive, in keyframe (sensor-orientation) pixels. */
data class Region(val x0: Int, val y0: Int, val x1: Int, val y1: Int)

/**
 * Face finder behind a small seam so BlazeFace can replace the platform detector later (ADR 0021).
 * [detect] gets the full-resolution NV21 buffer and returns face boxes in the same pixel space, or null when the
 * detector failed (redaction fails closed: that keyframe is not written and is recorded as dropped_detector_failure).
 */
interface FaceRegionDetector {
    val name: String
    fun detect(nv21: ByteArray, w: Int, h: Int): List<Region>?
}

/**
 * Face redaction v1: best effort, in memory, on the NV21 right before the JPEG is compressed, so no unredacted
 * keyframe pixels reach disk when it is on. Solid fill (Y dark, chroma neutral). Called from the single ck-jpeg
 * thread; [writeJson] runs after that thread has drained. Fails closed: when the detector throws or returns null the
 * entry is [Entry.dropped] and the caller must NOT write that keyframe. With [bundleDir] set, the json is also
 * rewritten every [FLUSH_EVERY] entries so a killed session keeps most of them.
 */
class Redactor(
    private val detector: FaceRegionDetector,
    private val bundleDir: File? = null,
    private val padFraction: Double = 0.15,
    private val onFailing: (() -> Unit)? = null,   // fired once per streak of FAILING_AFTER consecutive detector failures
) {
    class Entry(val file: String, val id: Int, val dropped: Boolean, val regions: List<Region>)

    private val entries = ArrayList<Entry>()
    private var failStreak = 0

    /** Redacts faces in [nv21] in place and records them. Never throws; check [Entry.dropped]. */
    @Synchronized
    fun redact(nv21: ByteArray, w: Int, h: Int, id: Int, file: String): Entry {
        val found = try { detector.detect(nv21, w, h) } catch (t: Throwable) { null }
        val e = if (found == null) Entry(file, id, true, emptyList())
        else {
            val padded = found.mapNotNull { pad(it, w, h) }
            for (r in padded) fill(nv21, w, h, r)
            Entry(file, id, false, padded)
        }
        entries.add(e)
        if (!e.dropped) failStreak = 0 else if (++failStreak == FAILING_AFTER) onFailing?.invoke()
        if (bundleDir != null && entries.size % FLUSH_EVERY == 0) writeJson(bundleDir)
        return e
    }

    @Synchronized fun entryCount() = entries.size

    @Synchronized
    fun toJson(): String = buildString {
        append("{\"version\":1,\"mode\":\"faces\",\"best_effort\":true,\"detector\":\"").append(detector.name)
        append("\",\"fill\":\"solid\",\"coords\":\"keyframe_pixels_x0y0x1y1\",\"keyframes\":[")
        entries.forEachIndexed { i, e ->
            if (i > 0) append(',')
            append("{\"id\":").append(e.id).append(",\"file\":\"").append(e.file.replace("\\", "\\\\").replace("\"", "\\\""))
            append("\",\"status\":\"").append(if (e.dropped) "dropped_detector_failure" else "ok").append("\",\"regions\":[")
            e.regions.forEachIndexed { j, r ->
                if (j > 0) append(',')
                append('[').append(r.x0).append(',').append(r.y0).append(',').append(r.x1).append(',').append(r.y1).append(']')
            }
            append("]}")
        }
        append("]}")
    }

    /**
     * Writes privacy/redaction.json under [dir]: stale tmp deleted, tmp written and fsynced, then moved over the
     * target. Never throws (returns false); call before finalize.
     */
    @Synchronized
    fun writeJson(dir: File): Boolean = try {
        val target = File(dir, "privacy/redaction.json")
        target.parentFile?.mkdirs()
        val tmp = File(dir, "privacy/redaction.json.tmp")
        tmp.delete()
        FileOutputStream(tmp).use { it.write(toJson().toByteArray(Charsets.UTF_8)); it.fd.sync() }
        Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING)
        true
    } catch (t: Throwable) { false }

    private fun pad(r: Region, w: Int, h: Int): Region? {
        val px = ((r.x1 - r.x0) * padFraction).toInt(); val py = ((r.y1 - r.y0) * padFraction).toInt()
        // Even bounds so the half-resolution chroma plane is covered exactly.
        val x0 = (maxOf(0, r.x0 - px)) and 1.inv(); val y0 = (maxOf(0, r.y0 - py)) and 1.inv()
        val x1 = minOf(w, (r.x1 + px + 1) and 1.inv()); val y1 = minOf(h, (r.y1 + py + 1) and 1.inv())
        return if (x1 > x0 && y1 > y0) Region(x0, y0, x1, y1) else null
    }

    companion object {
        const val FLUSH_EVERY = 10
        const val FAILING_AFTER = 5
        const val FILL_Y: Byte = 0x20
        const val FILL_C: Byte = 0x80.toByte()

        /** Solid fill of [r] (even bounds) in an NV21 buffer of w x h. */
        fun fill(nv21: ByteArray, w: Int, h: Int, r: Region) {
            for (y in r.y0 until r.y1) java.util.Arrays.fill(nv21, y * w + r.x0, y * w + r.x1, FILL_Y)
            val base = w * h
            for (y in r.y0 / 2 until r.y1 / 2) java.util.Arrays.fill(nv21, base + y * w + r.x0, base + y * w + r.x1, FILL_C)
        }

        /**
         * Maps a rect found in a base image rotated by [rot] degrees (0, 90 = clockwise, 270 = counter-clockwise;
         * rotated dims are bh x bw for 90/270) back to the unrotated base image of bw x bh.
         */
        fun unrotate(rot: Int, x0: Int, y0: Int, x1: Int, y1: Int, bw: Int, bh: Int): Region = when (rot) {
            90 -> Region(y0, bh - x1, y1, bh - x0)
            270 -> Region(bw - y1, x0, bw - y0, x1)
            else -> Region(x0, y0, x1, y1)
        }
    }
}

/**
 * android.media.FaceDetector on a downscaled grayscale RGB_565 copy (needs even width; fixed size per instance).
 * Phones hold the sensor sideways, so faces are scanned upright and rotated 90 degrees both ways. Scratch buffers
 * are reused. Recall and latency are unmeasured (ADR 0021): the manifest says best effort. Failures surface as null from detect.
 */
class PlatformFaceDetector(private val longSide: Int = 320, private val maxFaces: Int = 8) : FaceRegionDetector {
    override val name = "android.media.FaceDetector"

    /** One bitmap, pixel buffer and detector for one orientation, allocated once (again only if the size changes). */
    private class Ctx {
        var w = 0; var h = 0
        var bmp: android.graphics.Bitmap? = null
        var det: android.media.FaceDetector? = null
        var px = IntArray(0)
        fun ensure(nw: Int, nh: Int, maxFaces: Int) {
            if (bmp != null && w == nw && h == nh) return
            bmp?.recycle(); bmp = null; det = null   // never leave a recycled bitmap behind if the allocation below throws
            bmp = android.graphics.Bitmap.createBitmap(nw, nh, android.graphics.Bitmap.Config.RGB_565)
            det = android.media.FaceDetector(nw, nh, maxFaces)
            px = IntArray(nw * nh); w = nw; h = nh
        }
    }
    private val landscape = Ctx()   // rotation 0
    private val portrait = Ctx()    // rotations 90 and 270
    private val faces = arrayOfNulls<android.media.FaceDetector.Face>(maxFaces)
    private val mid = android.graphics.PointF()

    override fun detect(nv21: ByteArray, w: Int, h: Int): List<Region>? {
        if (w < 16 || h < 16) return emptyList()
        val scale = longSide.toDouble() / maxOf(w, h)
        val bw = ((w * scale).toInt() and 1.inv()).coerceAtLeast(2)
        val bh = ((h * scale).toInt() and 1.inv()).coerceAtLeast(2)
        val out = ArrayList<Region>()
        for (rot in ROTATIONS) {
            val c = if (rot == 0) landscape else portrait
            val dw = if (rot == 0) bw else bh; val dh = if (rot == 0) bh else bw
            c.ensure(dw, dh, maxFaces)
            val px = c.px
            var o = 0
            for (ry in 0 until dh) for (rx in 0 until dw) {
                val bx: Int; val by: Int
                when (rot) { 90 -> { bx = ry; by = bh - 1 - rx }; 270 -> { bx = bw - 1 - ry; by = rx }; else -> { bx = rx; by = ry } }
                val g = nv21[(by * h / bh) * w + bx * w / bw].toInt() and 0xFF
                px[o++] = (0xFF shl 24) or (g shl 16) or (g shl 8) or g
            }
            val b = c.bmp!!
            b.setPixels(px, 0, dw, 0, 0, dw, dh)
            java.util.Arrays.fill(faces, null)
            val n = c.det!!.findFaces(b, faces)
            for (i in 0 until n) {
                val f = faces[i] ?: continue
                if (f.confidence() < MIN_CONFIDENCE) continue
                f.getMidPoint(mid)
                val half = f.eyesDistance() * 2.0f   // square box around the eye midpoint, generous
                val r = Redactor.unrotate(
                    rot, (mid.x - half).toInt().coerceIn(0, dw), (mid.y - half).toInt().coerceIn(0, dh),
                    (mid.x + half).toInt().coerceIn(0, dw), (mid.y + half).toInt().coerceIn(0, dh), bw, bh,
                )
                out.add(Region(r.x0 * w / bw, r.y0 * h / bh, r.x1 * w / bw, r.y1 * h / bh))
            }
        }
        return out
    }

    private companion object {
        val ROTATIONS = intArrayOf(0, 90, 270)
        const val MIN_CONFIDENCE = 0.4f
    }
}
