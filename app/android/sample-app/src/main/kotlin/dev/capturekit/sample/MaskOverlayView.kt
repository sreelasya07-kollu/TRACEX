package dev.capturekit.sample

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.os.SystemClock
import android.util.Log
import android.view.View
import com.google.ar.core.Coordinates2d
import com.google.ar.core.Frame
import dev.capturekit.segment.CameraSnapshot
import dev.capturekit.segment.MaskComposer
import dev.capturekit.segment.MaskWarp
import dev.capturekit.segment.SegResult
import dev.capturekit.segment.SegmentationFeed

/**
 * Segmentation masks over the camera feed (issue #8): translucent per-track colour, the track's label at the mask
 * centroid; colour and label stay with the object across runs ([dev.capturekit.segment.TrackMemory]). Runs are <= 2 Hz,
 * so between runs the last masks are warped by the camera rotation since their run ([MaskWarp]) and mapped image ->
 * view with Frame.transformCoordinates2d, so crop and rotation match the background renderer. Masks older than
 * [STALE_MS] (no new run: busy, paused or lost tracking) are hidden. Must fill the same parent as the AR GLSurfaceView.
 * Masks are display-only and never written to the bundle.
 *
 * Threads: [setResult] on the segmentation thread, [onArFrame] on the GL thread, drawing on the UI thread. Each hand-over
 * is one immutable object behind a @Volatile reference: no lock, nothing blocks the AR thread.
 */
class MaskOverlayView(ctx: Context) : View(ctx) {
    private val d = ctx.resources.displayMetrics.density

    /** Segmentation thread -> GL thread: one run's raster + labels in camera-image pixels. */
    private class Pending(val bmp: Bitmap, val cam: CameraSnapshot, val labels: Array<String>, val colors: IntArray,
                          val anchors: FloatArray, val arrivedMs: Long)

    /** GL thread -> UI thread: where the raster corners and label anchors land in view pixels. */
    private class Mapped(val p: Pending, val corners: FloatArray, val anchors: FloatArray, val anchorOk: BooleanArray)

    @Volatile private var pending: Pending? = null
    @Volatile private var mapped: Mapped? = null
    private val imgPts = FloatArray(2 * (4 + MAX_LABELS))    // GL thread only
    private val viewPts = FloatArray(2 * (4 + MAX_LABELS))   // GL thread only
    private var mapErrors = 0L                               // GL thread only

    /** Off = nothing is mapped or drawn (the HUD "Masks" chip). Any thread. */
    @Volatile var showMasks = true
        set(v) { field = v; if (!v) { pending = null; mapped = null }; postInvalidate() }

    private val maskPaint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val tagBg = Paint(Paint.ANTI_ALIAS_FLAG)
    private val tagText = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 13f * d; color = 0xFF000000.toInt(); isFakeBoldText = true }
    private val m = Matrix()
    private val src = FloatArray(8)
    private val r = RectF()

    init {
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO   // visual hint only
        isClickable = false; isFocusable = false                     // touches fall through to the HUD / AR view
    }

    /** Segmentation thread: one run's masks. A run with no masks clears the overlay. */
    fun setResult(res: SegResult) {
        if (!showMasks) return
        val raster = MaskComposer.argb(res.masks, MASK_ALPHA)
        if (raster == null) { pending = null; mapped = null; postInvalidate(); return }
        val (w, h, px) = raster
        val bmp = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888)
        val n = minOf(res.masks.size, MAX_LABELS)
        val anchors = FloatArray(2 * n)
        for (i in 0 until n) { anchors[2 * i] = res.masks[i].cx * res.camera.w; anchors[2 * i + 1] = res.masks[i].cy * res.camera.h }
        pending = Pending(bmp, res.camera, Array(n) { res.masks[it].track.label }, IntArray(n) { res.masks[it].track.color },
            anchors, SystemClock.uptimeMillis())
    }

    /** GL thread, every AR frame: warps the last masks to the current camera and maps them to the view. Never throws. */
    fun onArFrame(frame: Frame) {
        val p = pending ?: return
        if (!showMasks) return
        if (SystemClock.uptimeMillis() - p.arrivedMs > STALE_MS) { pending = null; mapped = null; postInvalidate(); return }
        try {
            val now = SegmentationFeed.snapshot(frame.camera)
            val hgr = MaskWarp.homography(p.cam, now)
            val w = p.cam.w.toFloat(); val h = p.cam.h.toFloat()
            val corners = floatArrayOf(0f, 0f, w, 0f, w, h, 0f, h)
            for (i in 0 until 4) if (!MaskWarp.apply(hgr, corners[2 * i], corners[2 * i + 1], imgPts, 2 * i)) { mapped = null; postInvalidate(); return }
            val n = p.labels.size
            val ok = BooleanArray(n)
            for (i in 0 until n) ok[i] = MaskWarp.apply(hgr, p.anchors[2 * i], p.anchors[2 * i + 1], imgPts, 8 + 2 * i)
            val count = 2 * (4 + n)
            frame.transformCoordinates2d(Coordinates2d.IMAGE_PIXELS, imgPts.copyOf(count), Coordinates2d.VIEW, viewPts)
            mapped = Mapped(p, viewPts.copyOfRange(0, 8), viewPts.copyOfRange(8, count), ok)
            postInvalidate()
        } catch (t: Throwable) {   // e.g. a frame without an image yet: skip, never break the AR callback
            if (mapErrors++ % 100 == 0L) Log.w(TAG, "mask mapping failed ($mapErrors)", t)
        }
    }

    /** Drops everything (scan finished or paused). Any thread. */
    fun clear() { pending = null; mapped = null; postInvalidate() }

    override fun onDraw(c: Canvas) {
        val mp = mapped ?: return
        if (!showMasks) return
        val bmp = mp.p.bmp
        src[0] = 0f; src[1] = 0f; src[2] = bmp.width.toFloat(); src[3] = 0f
        src[4] = bmp.width.toFloat(); src[5] = bmp.height.toFloat(); src[6] = 0f; src[7] = bmp.height.toFloat()
        if (!m.setPolyToPoly(src, 0, mp.corners, 0, 4)) return
        c.drawBitmap(bmp, m, maskPaint)
        val pad = 4f * d
        val fm = tagText.fontMetrics
        val tagH = fm.descent - fm.ascent + pad
        for (i in mp.p.labels.indices) {
            if (!mp.anchorOk[i]) continue
            val x = mp.anchors[2 * i]; val y = mp.anchors[2 * i + 1]
            if (x < 0 || y < 0 || x > width || y > height) continue
            val label = mp.p.labels[i]
            val tw = tagText.measureText(label) + 2 * pad
            tagBg.color = mp.p.colors[i]
            r.set(x - tw / 2, y - tagH / 2, x + tw / 2, y + tagH / 2)
            c.drawRoundRect(r, 4f * d, 4f * d, tagBg)
            c.drawText(label, x - tw / 2 + pad, y - tagH / 2 + pad / 2 - fm.ascent, tagText)
        }
    }

    private companion object {
        const val TAG = "CKMasks"
        const val MASK_ALPHA = 0x66        // ~40 % so the camera stays readable
        const val STALE_MS = 3_000L        // guess; tune on device (8b)
        const val MAX_LABELS = 8
    }
}
