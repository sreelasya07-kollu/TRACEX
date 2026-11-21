package dev.capturekit.sample

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.view.animation.LinearInterpolator
import android.widget.LinearLayout
import android.widget.TextView
import dev.capturekit.guidance.Recalibration
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * Issue #1 / ADR 0031: the centred "Recalibrating" card shown while the core recovers tracking. A phone icon circles
 * slowly (the motion the user should make), with the core's sentence underneath. It never takes touches, so Pause and
 * Finish stay usable behind it. Wording comes from the core ([Recalibration.prompt]); only the title is a fixed label.
 * UI thread only.
 */
class RecalibrateOverlay(ctx: Context) {
    private val d = ctx.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private val anim = PhoneCircleView(ctx)
    private val title = TextView(ctx).apply {
        text = Recalibration.TITLE; textSize = UiKit.TEXT_L; setTextColor(Color.WHITE); typeface = Typeface.DEFAULT_BOLD
        gravity = Gravity.CENTER
    }
    private val body = TextView(ctx).apply {
        textSize = UiKit.TEXT_M; setTextColor(Color.WHITE); gravity = Gravity.CENTER; maxLines = 3
    }
    val card: View = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER_HORIZONTAL
        background = UiKit.glass(ctx)
        setPadding(dp(20), dp(16), dp(20), dp(16))
        visibility = View.GONE
        isClickable = false; isFocusable = false
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
        addView(anim, LinearLayout.LayoutParams(dp(112), dp(112)))
        addView(title, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(8) })
        addView(body, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(4) })
    }

    init {
        val longSide = maxOf(ctx.resources.displayMetrics.widthPixels, ctx.resources.displayMetrics.heightPixels)
        body.maxWidth = (longSide * 0.45).toInt()
    }

    /** Shows the card with [text] (core long_text), or hides it for null. No-op when unchanged. */
    fun set(text: String?) {
        if (text == null) {
            if (card.visibility != View.GONE) { card.visibility = View.GONE; anim.stop() }
            return
        }
        if (body.text.toString() != text) {
            body.text = text
            card.contentDescription = "${Recalibration.TITLE}. $text"
        }
        if (card.visibility != View.VISIBLE) {
            card.visibility = View.VISIBLE
            anim.start()
            card.announceForAccessibility(card.contentDescription)   // TalkBack users hear it even with voice guidance off
        }
    }

    fun release() = anim.stop()

    /** A phone outline travelling around a dashed circle, tilting as it goes: "turn your phone in a small circle". */
    private class PhoneCircleView(ctx: Context) : View(ctx) {
        private val density = ctx.resources.displayMetrics.density
        private val track = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE; strokeWidth = 2 * density; color = 0x80FFFFFF.toInt()
            pathEffect = DashPathEffect(floatArrayOf(6 * density, 6 * density), 0f)
        }
        private val body = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0xFF263238.toInt() }
        private val edge = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 2.5f * density; color = Color.WHITE }
        private val lens = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = UiKit.AMBER }
        private val phone = RectF()
        private var angle = 0f   // radians, clockwise on screen
        private val animator = ValueAnimator.ofFloat(0f, (2 * Math.PI).toFloat()).apply {
            duration = PERIOD_MS; repeatCount = ValueAnimator.INFINITE; interpolator = LinearInterpolator()
            addUpdateListener { angle = it.animatedValue as Float; invalidate() }
        }

        fun start() { if (!animator.isStarted) animator.start() }
        fun stop() { animator.cancel() }

        override fun onDetachedFromWindow() { stop(); super.onDetachedFromWindow() }

        override fun onDraw(c: Canvas) {
            super.onDraw(c)
            val cx = width / 2f; val cy = height / 2f
            val s = min(width, height).toFloat()
            val r = s * 0.24f
            c.drawCircle(cx, cy, r, track)
            val px = cx + r * cos(angle); val py = cy + r * sin(angle)
            val w = s * 0.2f; val h = s * 0.34f
            c.save()
            c.rotate(15f * sin(angle), px, py)   // a gentle wrist tilt while circling
            phone.set(px - w / 2, py - h / 2, px + w / 2, py + h / 2)
            val rad = w * 0.22f
            c.drawRoundRect(phone, rad, rad, body)
            c.drawRoundRect(phone, rad, rad, edge)
            c.drawCircle(px, py - h * 0.32f, w * 0.1f, lens)   // the camera: what has to see the room
            c.restore()
        }

        companion object { const val PERIOD_MS = 2400L }   // ~2.6 rad/s on screen: an unhurried circle
    }
}
