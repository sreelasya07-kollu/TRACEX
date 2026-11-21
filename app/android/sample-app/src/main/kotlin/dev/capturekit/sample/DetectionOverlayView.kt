package dev.capturekit.sample

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.os.SystemClock
import android.util.Log
import android.view.View
import com.google.ar.core.Coordinates2d
import com.google.ar.core.Frame
import dev.capturekit.capture.DetectedBox
import dev.capturekit.capture.DetectionDisplay
import dev.capturekit.capture.LabelMap

/**
 * Detector boxes over the camera feed (issue #2 C): each keyframe's 2D boxes with "class score%" labels, mapped from
 * the stored keyframe (= ARCore CPU image) to this view with Frame.transformCoordinates2d(IMAGE_PIXELS -> VIEW), so
 * crop and rotation always match what the background renderer shows. The boxes come from a keyframe that is up to a
 * detector interval old, so they are shown for [DetectionDisplay.SHOW_MS] and fade (a hint, not a tracker).
 * Must fill the same parent as the AR GLSurfaceView (VIEW coordinates are the AR view's).
 *
 * Threads: [setDetections] on the detector thread, [onArFrame] on the GL thread, drawing on the UI thread. Each hand-over
 * is one immutable object behind a @Volatile reference, so no lock is needed and nothing blocks the AR thread.
 */
class DetectionOverlayView(ctx: Context) : View(ctx) {
    private val d = ctx.resources.displayMetrics.density

    /** Detector thread -> GL thread: one keyframe's boxes in stored pixels, with labels already built. */
    private class Pending(val corners: FloatArray, val n: Int, val labels: Array<String>, val colors: IntArray, val arrivedMs: Long)

    /** GL thread -> UI thread: view rectangles (l, t, r, b) ready to draw. */
    private class Mapped(val rects: FloatArray, val n: Int, val labels: Array<String>, val colors: IntArray, val arrivedMs: Long)

    @Volatile private var pending: Pending? = null
    @Volatile private var mapped: Mapped? = null
    private var mappedFrom: Pending? = null           // GL thread only: which Pending [mapped] was built from
    private var mappedW = 0; private var mappedH = 0  // GL thread only: view size [mapped] was built for
    private val viewCorners = FloatArray(4 * DetectionDisplay.MAX_BOXES)   // GL thread only
    private var mapErrors = 0L                        // GL thread only

    /** Off = nothing is mapped or drawn (the HUD "Boxes" chip). Any thread. */
    @Volatile var showBoxes = true
        set(v) { field = v; if (!v) { pending = null; mapped = null }; postInvalidate() }

    private val box = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 2.5f * d }
    private val tagBg = Paint(Paint.ANTI_ALIAS_FLAG)
    private val tagText = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 13f * d; color = 0xFF000000.toInt(); isFakeBoldText = true }
    private val r = RectF()

    init {
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO   // visual hint only; the review screen lists what was found
        isClickable = false; isFocusable = false                     // touches fall through to the HUD / AR view
    }

    /** Detector thread: the boxes of one keyframe (stored pixels). An empty list clears the boxes (ran, saw nothing). */
    fun setDetections(boxes: List<DetectedBox>) {
        if (!showBoxes) return
        val corners = FloatArray(4 * DetectionDisplay.MAX_BOXES)
        val n = DetectionDisplay.corners(boxes, corners)
        pending = Pending(corners, n, Array(n) { LabelMap.boxLabel(boxes[it].cls, boxes[it].score) },
            IntArray(n) { RoomView.objectColor(LabelMap.coreName(boxes[it].cls)) }, SystemClock.uptimeMillis())
    }

    /** GL thread, every AR frame: maps new boxes (or all of them again after a display rotation / resize). Never throws. */
    fun onArFrame(frame: Frame) {
        val p = pending ?: return
        if (!showBoxes) return
        // Remap on new boxes, on ARCore's one-frame geometry flag, or when the view size changed since the last mapping
        // (a rotation whose flagged frame this callback missed).
        val w = width; val h = height
        if (p === mappedFrom && !frame.hasDisplayGeometryChanged() && w == mappedW && h == mappedH) return
        mappedFrom = p; mappedW = w; mappedH = h
        if (p.n == 0) { mapped = null; postInvalidate(); return }
        try {
            frame.transformCoordinates2d(Coordinates2d.IMAGE_PIXELS, p.corners, Coordinates2d.VIEW, viewCorners)
        } catch (t: Throwable) {   // e.g. a frame without an image yet: skip this set, never break the AR callback
            if (mapErrors++ % 100 == 0L) Log.w(TAG, "box mapping failed ($mapErrors)", t)
            return
        }
        val rects = FloatArray(4 * p.n)
        val keep = IntArray(p.n)
        val k = DetectionDisplay.rects(viewCorners, p.n, rects, keep)
        mapped = Mapped(rects, k, Array(k) { p.labels[keep[it]] }, IntArray(k) { p.colors[keep[it]] }, p.arrivedMs)
        postInvalidate()
    }

    /** Drops everything (scan finished or paused). Any thread. */
    fun clear() { pending = null; mapped = null; postInvalidate() }

    override fun onDraw(c: Canvas) {
        val m = mapped ?: return
        if (!showBoxes) return
        val a = DetectionDisplay.alpha(SystemClock.uptimeMillis() - m.arrivedMs)
        if (a <= 0f) return
        val alpha = (255 * a).toInt()
        val pad = 4f * d
        val fm = tagText.fontMetrics
        val tagH = fm.descent - fm.ascent + pad
        for (i in 0 until m.n) {
            val l = m.rects[4 * i]; val t = m.rects[4 * i + 1]; val rr = m.rects[4 * i + 2]; val b = m.rects[4 * i + 3]
            box.color = m.colors[i]; box.alpha = alpha
            r.set(l, t, rr, b)
            c.drawRoundRect(r, 6f * d, 6f * d, box)
            // Label tag above the top edge (inside the box when the box touches the top of the view).
            val label = m.labels[i]
            val tw = tagText.measureText(label) + 2 * pad
            val ty = if (t - tagH >= 0f) t - tagH else t
            tagBg.color = m.colors[i]; tagBg.alpha = alpha
            r.set(l, ty, l + tw, ty + tagH)
            c.drawRoundRect(r, 4f * d, 4f * d, tagBg)
            tagText.alpha = alpha
            c.drawText(label, l + pad, ty + pad / 2 - fm.ascent, tagText)
        }
        postInvalidateOnAnimation()   // keep fading until alpha reaches 0, then onDraw returns early
    }

    private companion object { const val TAG = "CKBoxes" }
}
