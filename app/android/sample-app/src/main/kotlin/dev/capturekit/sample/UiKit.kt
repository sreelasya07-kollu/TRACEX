package dev.capturekit.sample

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.content.pm.PackageManager
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.Outline
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView
import dev.capturekit.guidance.HapticMap
import dev.capturekit.nativebridge.Status
import java.io.File
import java.util.Locale
import kotlin.math.min

/**
 * CaptureKit design system: monochrome (Apple / Notion), Liquid Glass surfaces, one type scale.
 * Colour only carries meaning (status); everything else is white at different opacities over near-black.
 */
object UiKit {
    // ---- Palette ----
    const val BG = 0xFF0A0A0B.toInt()
    const val LABEL = 0xFFF5F5F7.toInt()          // primary text
    const val LABEL_2 = 0xA6EBEBF5.toInt()        // secondary text (65%)
    const val LABEL_3 = 0x61EBEBF5                // tertiary text / captions (38%)
    const val SEPARATOR = 0x1AFFFFFF
    const val FILL = 0x14FFFFFF                   // icon tiles, quiet fills

    // Status only (Apple dark-mode system colours).
    const val GREEN = 0xFF30D158.toInt()
    const val AMBER = 0xFFFF9F0A.toInt()
    const val RED = 0xFFFF453A.toInt()
    /** Primary action: off-white with black text. (Name kept for existing call sites; there is no blue accent.) */
    const val BLUE_ACCENT = LABEL
    const val PRIMARY = LABEL
    /** "In progress" ink for live room data; neutral white, no hue. */
    const val CYAN_ACCENT = 0xFFE5E5EA.toInt()
    const val PANEL = 0xFF3A3A3C.toInt()

    // ---- Glass ----
    const val GLASS = 0x8C2C2C2E.toInt()          // glass body: translucent graphite, reads on black and over the camera
    const val GLASS_STRONG = 0xB32C2C2E.toInt()   // over busy camera content
    const val BORDER_STROKE = 0x1FFFFFFF          // one even hairline edge

    // ---- Shape / type ----
    const val RADIUS_DP = 22
    const val RADIUS_PILL_DP = 999
    const val TEXT_S = 13f
    const val TEXT_M = 16f
    const val TEXT_L = 20f
    val MEDIUM: Typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    val BOLD: Typeface = Typeface.create("sans-serif", Typeface.BOLD)

    enum class Kind(val color: Int) { OK(GREEN), WARN(AMBER), ERROR(RED), INFO(PANEL) }

    fun Context.dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
    fun Context.dpF(v: Float): Float = v * resources.displayMetrics.density

    private fun isStatus(c: Int) = c == GREEN || c == AMBER || c == RED ||
        (Color.alpha(c) == 0xFF && saturation(c) > 0.35f && c != PRIMARY)
    private fun saturation(c: Int): Float { val hsv = FloatArray(3); Color.colorToHSV(c, hsv); return hsv[1] }
    private fun withAlpha(c: Int, a: Int) = (c and 0x00FFFFFF) or (a shl 24)

    /** Glass body for [color]: primary = solid off-white; status = glass tinted by the hue; anything else = plain glass. */
    private fun surfaceFor(ctx: Context, radiusDp: Int, color: Int): Drawable {
        val r = ctx.dpF(radiusDp.toFloat())
        val d = ctx.resources.displayMetrics.density
        return when {
            color == PRIMARY -> LiquidGlass(r, d, fill = PRIMARY, rimStrength = 0.5f)
            isStatus(color) -> LiquidGlass(r, d, fill = GLASS, tint = withAlpha(color, 0x3D))
            else -> LiquidGlass(r, d, fill = GLASS)
        }
    }

    /** Liquid Glass panel. */
    fun glass(ctx: Context, radiusDp: Int = RADIUS_DP, @Suppress("UNUSED_PARAMETER") strokeColor: Int = BORDER_STROKE, strokeWidthDp: Int = 1): Drawable =
        LiquidGlass(ctx.dpF(radiusDp.toFloat()), ctx.resources.displayMetrics.density, fill = GLASS, rimStrength = if (strokeWidthDp > 0) 1f else 0f)

    /** Capsule glass. */
    fun nowBar(ctx: Context, @Suppress("UNUSED_PARAMETER") bgColor: Int = GLASS): Drawable =
        LiquidGlass(ctx.dpF(RADIUS_PILL_DP.toFloat()), ctx.resources.displayMetrics.density, fill = GLASS_STRONG)

    fun rounded(color: Int, radiusPx: Float, strokeColor: Int = 0, strokeWidthPx: Int = 0): GradientDrawable =
        GradientDrawable().apply {
            setColor(color)
            cornerRadius = radiusPx
            if (strokeWidthPx > 0) setStroke(strokeWidthPx, strokeColor)
        }

    /** Glass card. The colour arguments are kept for call-site compatibility; cards are always neutral glass. */
    @Suppress("UNUSED_PARAMETER")
    fun card(ctx: Context, radiusDp: Int = 24, bgColor: Int = GLASS, strokeColor: Int = BORDER_STROKE): Drawable =
        LiquidGlass(ctx.dpF(radiusDp.toFloat()), ctx.resources.displayMetrics.density, fill = GLASS)

    /** Tappable glass with a soft white ripple. */
    fun rippleSurface(ctx: Context, radiusDp: Int, bgColor: Int, @Suppress("UNUSED_PARAMETER") strokeColor: Int = BORDER_STROKE): RippleDrawable {
        val mask = GradientDrawable().apply { setColor(Color.WHITE); cornerRadius = ctx.dpF(radiusDp.toFloat()) }
        val ripple = if (bgColor == PRIMARY) 0x22000000 else 0x1FFFFFFF
        return RippleDrawable(ColorStateList.valueOf(ripple), surfaceFor(ctx, radiusDp, bgColor), mask)
    }

    /** Ripple only (rows inside a grouped list). */
    fun rowRipple(): RippleDrawable = RippleDrawable(ColorStateList.valueOf(0x1AFFFFFF), null, ColorDrawable(Color.WHITE))

    fun iconImage(ctx: Context, drawableId: Int, tint: Int): ImageView = ImageView(ctx).apply {
        setImageResource(drawableId)
        imageTintList = ColorStateList.valueOf(tint)
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        scaleType = ImageView.ScaleType.FIT_CENTER
    }

    fun circularIconButton(ctx: Context, drawableId: Int, description: String, sizeDp: Int = 44, bgColor: Int = GLASS): FrameLayout =
        FrameLayout(ctx).apply {
            isClickable = true
            isFocusable = true
            minimumWidth = ctx.dp(sizeDp)
            minimumHeight = ctx.dp(sizeDp)
            contentDescription = description
            background = rippleSurface(ctx, RADIUS_PILL_DP, bgColor)
            addView(iconImage(ctx, drawableId, LABEL), FrameLayout.LayoutParams(ctx.dp(20), ctx.dp(20), Gravity.CENTER))
        }

    fun statusText(ctx: Context, sizeSp: Float = 16f): TextView = TextView(ctx).apply {
        textSize = sizeSp
        setTextColor(LABEL)
        background = glass(ctx, 16)
        setPadding(ctx.dp(14), ctx.dp(10), ctx.dp(14), ctx.dp(10))
    }

    /** Capsule button. [color] PRIMARY = off-white with black text (the one prominent action); others = glass. */
    fun bigButton(ctx: Context, label: String, color: Int = PRIMARY): Button = Button(ctx).apply {
        text = label
        textSize = 17f
        isAllCaps = false
        stateListAnimator = null
        typeface = MEDIUM
        setTextColor(if (color == PRIMARY) Color.BLACK else LABEL)
        background = rippleSurface(ctx, RADIUS_PILL_DP, color)
        minimumHeight = ctx.dp(54)
        minHeight = ctx.dp(54)
        setPadding(ctx.dp(24), ctx.dp(12), ctx.dp(24), ctx.dp(12))
        pressScale(this)
    }

    /** Circular glass button. */
    fun circularButton(ctx: Context, iconText: String, sizeDp: Int = 48, bgColor: Int = GLASS): Button =
        Button(ctx).apply {
            text = iconText
            textSize = 18f
            setTextColor(LABEL)
            typeface = MEDIUM
            isAllCaps = false
            stateListAnimator = null
            background = rippleSurface(ctx, RADIUS_PILL_DP, bgColor)
            minimumWidth = ctx.dp(sizeDp)
            minWidth = ctx.dp(sizeDp)
            minimumHeight = ctx.dp(sizeDp)
            minHeight = ctx.dp(sizeDp)
            setPadding(0, 0, 0, 0)
            gravity = Gravity.CENTER
            pressScale(this)
        }

    /** Small quiet capsule (tags, status). */
    @Suppress("UNUSED_PARAMETER")
    fun pillBadge(ctx: Context, label: String, bgColor: Int, textColor: Int, strokeColor: Int = 0): TextView =
        TextView(ctx).apply {
            text = label
            textSize = 12f
            setTextColor(if (isStatus(textColor)) textColor else LABEL_2)
            typeface = MEDIUM
            gravity = Gravity.CENTER
            background = rounded(if (isStatus(bgColor)) withAlpha(bgColor, 0x2E) else FILL, ctx.dpF(999f))
            setPadding(ctx.dp(10), ctx.dp(4), ctx.dp(10), ctx.dp(4))
        }

    fun recolor(button: Button, color: Int) {
        button.background = rippleSurface(button.context, RADIUS_PILL_DP, color)
        button.setTextColor(if (color == PRIMARY) Color.BLACK else LABEL)
    }

    /** Capsule chip (text set by [setChip]). */
    fun chip(ctx: Context): TextView = TextView(ctx).apply {
        textSize = 14f
        setTextColor(LABEL)
        typeface = MEDIUM
        gravity = Gravity.CENTER_VERTICAL
        setPadding(ctx.dp(14), ctx.dp(8), ctx.dp(14), ctx.dp(8))
    }

    /** Glass chip; the hue shows only as a small status dot and a faint tint, so the text stays readable. */
    fun setChip(v: TextView, text: String, color: Int) {
        if (v.tag == color && v.text.toString().removePrefix("● ") == text.removePrefix("● ")) return
        v.tag = color
        val status = isStatus(color)
        val body = text.removePrefix("● ")
        val shown = if (status || text.startsWith("● ")) "● $body" else body
        v.text = SpannableString(shown).apply {
            if (shown.startsWith("●")) setSpan(ForegroundColorSpan(if (status) color else LABEL_2), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        v.setTextColor(LABEL)
        v.background = rippleSurface(v.context, RADIUS_PILL_DP, color)
    }

    /** Message strip: glass with a faint status tint and a coloured dot. */
    fun banner(ctx: Context, msg: String, kind: Kind): TextView = TextView(ctx).apply { applyBanner(this, msg, kind) }

    fun applyBanner(v: TextView, msg: String, kind: Kind) {
        val ctx = v.context
        v.text = if (kind == Kind.INFO) msg else SpannableString("●  $msg").apply {
            setSpan(ForegroundColorSpan(kind.color), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        v.textSize = 16f
        v.setTextColor(LABEL)
        v.background = LiquidGlass(ctx.dpF(18f), ctx.resources.displayMetrics.density, fill = GLASS_STRONG,
            tint = if (kind == Kind.INFO) 0 else withAlpha(kind.color, 0x2E))
        v.setPadding(ctx.dp(16), ctx.dp(12), ctx.dp(16), ctx.dp(12))
    }

    // ---- Type ----
    fun largeTitle(ctx: Context, s: String): TextView = text(ctx, s, 32f, LABEL, BOLD).apply {
        letterSpacing = -0.02f
        if (Build.VERSION.SDK_INT >= 28) isAccessibilityHeading = true
    }
    fun title(ctx: Context, s: String, size: Float = 22f): TextView = text(ctx, s, size, LABEL, BOLD).apply { letterSpacing = -0.01f }
    fun body(ctx: Context, s: String, color: Int = LABEL_2, size: Float = 15f): TextView = text(ctx, s, size, color).apply { setLineSpacing(0f, 1.2f) }
    fun caption(ctx: Context, s: String): TextView = text(ctx, s, 13f, LABEL_3)
    fun text(ctx: Context, s: CharSequence, size: Float, color: Int = LABEL, face: Typeface = Typeface.DEFAULT): TextView = TextView(ctx).apply {
        text = s; textSize = size; setTextColor(color); typeface = face
    }

    /** "● text" with only the dot coloured. */
    fun dotted(s: String, color: Int): CharSequence = SpannableString("●  $s").apply {
        setSpan(ForegroundColorSpan(color), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
    }

    // ---- Grouped list (iOS inset-grouped / Notion settings) ----
    fun sectionHeader(ctx: Context, s: String): TextView = text(ctx, s, 13f, LABEL_3, MEDIUM).apply {
        setPadding(ctx.dp(16), 0, ctx.dp(16), ctx.dp(8))
        if (Build.VERSION.SDK_INT >= 28) isAccessibilityHeading = true
    }

    /** Glass group whose children are separated by hairlines. */
    fun group(ctx: Context): LinearLayout = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        background = card(ctx, RADIUS_DP)
        clipToOutline = true
        showDividers = LinearLayout.SHOW_DIVIDER_MIDDLE
        dividerDrawable = GradientDrawable().apply { setColor(SEPARATOR); setSize(1, maxOf(1, ctx.dp(1) / 2)) }
        dividerPadding = 0
    }

    /** One row: optional icon tile, title + subtitle, trailing view (chevron when tappable and nothing else given). */
    fun listRow(
        ctx: Context, icon: Int?, title: CharSequence, subtitle: CharSequence? = null,
        trailing: View? = null, titleColor: Int = LABEL, chevron: Boolean = true, onClick: (() -> Unit)? = null,
    ): LinearLayout = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = ctx.dp(56)
        setPadding(ctx.dp(16), ctx.dp(12), ctx.dp(14), ctx.dp(12))
        if (icon != null) addView(iconImage(ctx, icon, LABEL_2),
            LinearLayout.LayoutParams(ctx.dp(20), ctx.dp(20)).apply { rightMargin = ctx.dp(16) })
        addView(LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            addView(text(ctx, title, 16f, titleColor, if (titleColor == LABEL) MEDIUM else Typeface.DEFAULT))
            if (subtitle != null) addView(text(ctx, subtitle, 13.5f, LABEL_2).apply { setPadding(0, ctx.dp(2), 0, 0) })
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        val end = trailing ?: if (onClick != null && chevron) iconImage(ctx, R.drawable.ic_chevron_right, LABEL_3) else null
        if (end != null) addView(end, if (trailing == null) LinearLayout.LayoutParams(ctx.dp(18), ctx.dp(18)).apply { leftMargin = ctx.dp(8) }
            else LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { leftMargin = ctx.dp(8) })
        if (onClick != null) {
            isClickable = true; isFocusable = true
            foreground = rowRipple()
            setOnClickListener { onClick() }
        }
    }

    /** iOS-style switch: white thumb, green track when on. */
    fun toggle(ctx: Context, checked: Boolean): Switch = Switch(ctx).apply {
        isChecked = checked
        val states = arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf())
        thumbTintList = ColorStateList(states, intArrayOf(Color.WHITE, 0xFFE5E5EA.toInt()))
        trackTintList = ColorStateList(states, intArrayOf(GREEN, 0x52787880))
        if (Build.VERSION.SDK_INT >= 29) trackTintMode = android.graphics.PorterDuff.Mode.SRC
    }

    /** Full-screen page: flat near-black behind [content]. */
    fun page(ctx: Context, content: View): FrameLayout = FrameLayout(ctx).apply {
        setBackgroundColor(BG)
        addView(content, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
    }

    /** Springy press feedback (scale 0.97) without consuming the touch. */
    fun pressScale(v: View) {
        v.setOnTouchListener { view, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> view.animate().scaleX(0.97f).scaleY(0.97f).setDuration(90).start()
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> view.animate().scaleX(1f).scaleY(1f).setDuration(160).start()
            }
            false
        }
    }

    fun statusName(st: Int): String = when (st) {
        Status.OK -> "OK"; Status.STATE -> "STATE"; Status.ARG -> "ARG"
        Status.IO -> "IO"; Status.POLICY -> "POLICY"; Status.FULL -> "FULL"
        else -> "status $st"
    }

    fun mmss(ms: Long): String { val s = ms / 1000; return String.format(Locale.US, "%02d:%02d", s / 60, s % 60) }

    fun dirSizeBytes(dir: File): Long = dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }

    fun humanSize(b: Long): String = when {
        b >= 1L shl 20 -> String.format(Locale.US, "%.1f MB", b / 1048576.0)
        b >= 1L shl 10 -> String.format(Locale.US, "%.0f KB", b / 1024.0)
        else -> "$b B"
    }

    /** Pads [v] by the system bars/cutout (targetSdk 37 is edge-to-edge) plus [basePadDp]. */
    fun applyInsets(v: View, basePadDp: Int = 12) {
        val base = v.context.dp(basePadDp)
        v.setPadding(base, base, base, base)
        v.setOnApplyWindowInsetsListener { view, ins ->
            if (Build.VERSION.SDK_INT >= 30) {
                val b = ins.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                view.setPadding(base + b.left, base + b.top, base + b.right, base + b.bottom)
            }
            ins
        }
        v.requestApplyInsets()
    }

    fun haptic(v: View, confirm: Boolean) {
        val c = if (confirm && Build.VERSION.SDK_INT >= 30) HapticFeedbackConstants.CONFIRM else HapticFeedbackConstants.CONTEXT_CLICK
        v.performHapticFeedback(c)
    }

    /** Plays the core's Instruction.haptic ("none"/"tap"/"double"/"success") via [HapticMap]. Safe from any thread. */
    fun hapticFor(v: View, haptic: String, priority: Int) {
        for (p in HapticMap.pulses(haptic, priority)) {
            val run = Runnable { haptic(v, p.kind == HapticMap.Kind.CONFIRM) }
            if (p.delayMs == 0L) v.post(run) else v.postDelayed(run, p.delayMs)
        }
    }

    /** Hides the system bars (swipe brings them back briefly) so the preview is edge to edge. */
    fun immersive(a: Activity) {
        if (Build.VERSION.SDK_INT >= 30) {
            val lp = a.window.attributes
            if (lp.layoutInDisplayCutoutMode != WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS) {
                lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                a.window.attributes = lp
            }
        }
        if (Build.VERSION.SDK_INT >= 30) {
            a.window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            a.window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        }
    }

    /** Quiet secondary button (glass), same shape as [bigButton]. */
    fun secondaryButton(ctx: Context, label: String): Button = bigButton(ctx, label, GLASS)

    /** Text that keeps one or two lines and cuts the middle (long file paths). */
    fun pathText(ctx: Context, text: String, lines: Int = 2): TextView = TextView(ctx).apply {
        this.text = text; textSize = 13f; setTextColor(LABEL_3)
        maxLines = lines; ellipsize = android.text.TextUtils.TruncateAt.MIDDLE
    }
}

/**
 * Liquid Glass surface: a flat translucent body (optionally tinted by a status hue) and one even hairline edge.
 * No gradients, sheens or glows. Pure Canvas, cheap enough to sit over the AR camera on every phone (API 26+).
 */
class LiquidGlass(
    private val radius: Float,
    density: Float,
    fill: Int,
    tint: Int = 0,
    private val rimStrength: Float = 1f,
) : Drawable() {
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = fill }
    private val tintPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = tint }
    private val solid = Color.alpha(fill) == 0xFF
    private val rimPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = maxOf(1f, 0.75f * density)
        color = UiKit.BORDER_STROKE
    }
    private val rect = RectF()

    override fun draw(c: Canvas) {
        val b = bounds
        val half = rimPaint.strokeWidth / 2f
        rect.set(b.left + half, b.top + half, b.right - half, b.bottom - half)
        val r = min(radius, rect.height() / 2f)
        c.drawRoundRect(rect, r, r, fillPaint)
        if (Color.alpha(tintPaint.color) > 0) c.drawRoundRect(rect, r, r, tintPaint)
        if (!solid && rimStrength > 0f) c.drawRoundRect(rect, r, r, rimPaint)
    }

    override fun getOutline(outline: Outline) {
        val b = bounds
        outline.setRoundRect(b, min(radius, b.height() / 2f))
    }

    override fun setAlpha(alpha: Int) { fillPaint.alpha = alpha }
    override fun setColorFilter(colorFilter: ColorFilter?) { fillPaint.colorFilter = colorFilter }
    @Deprecated("Deprecated in Java")
    override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
}

/** Camera shutter with a distinct recording state, touch animation, and haptic feedback. */
class ShutterButton(ctx: Context) : Button(ctx) {
    private val d = ctx.resources.displayMetrics.density
    private fun dp(v: Float) = v * d

    var isRecording = false
        set(value) {
            field = value
            contentDescription = if (value) "Finish Scan" else "Start Scan"
            invalidate()
        }

    private val outerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(4f)
        color = 0xF2FFFFFF.toInt()
    }

    private val innerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = UiKit.RED // record red (the only colour on the capture deck)
    }

    private val stopSquarePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = UiKit.RED
    }

    private val rectF = RectF()

    init {
        background = null
        text = ""
        isClickable = true
        isFocusable = true
        contentDescription = "Start Scan"
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                animate().scaleX(0.92f).scaleY(0.92f).setDuration(100).start()
                performHapticFeedback(HapticFeedbackConstants.CONTEXT_CLICK)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                animate().scaleX(1.0f).scaleY(1.0f).setDuration(120).start()
            }
        }
        return super.onTouchEvent(event)
    }

    override fun onDraw(canvas: Canvas) {
        val cx = width / 2f
        val cy = height / 2f
        val outerRadius = (min(width, height) / 2f) - dp(2f)

        // Outer white camera ring
        canvas.drawCircle(cx, cy, outerRadius, outerPaint)

        if (!isRecording) {
            // Idle camera record button: solid red circle
            val innerRadius = outerRadius - dp(7f)
            canvas.drawCircle(cx, cy, innerRadius, innerPaint)
        } else {
            // Active recording stop glyph: rounded square in the center
            val halfSize = dp(13f)
            val cornerRadius = dp(6f)
            rectF.set(cx - halfSize, cy - halfSize, cx + halfSize, cy + halfSize)
            canvas.drawRoundRect(rectF, cornerRadius, cornerRadius, stopSquarePaint)
        }
    }
}

/** Spatial viewfinder corner-bracket reticle. */
class ReticleView(ctx: Context) : View(ctx) {
    private val d = ctx.resources.displayMetrics.density
    private fun dp(v: Float) = v * d

    private val bracketPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(2f)
        color = 0x80FFFFFF.toInt()
        strokeCap = Paint.Cap.ROUND
    }

    private val dotPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = 0xCCFFFFFF.toInt()
    }

    private val path = Path()

    override fun onDraw(canvas: Canvas) {
        val cx = width / 2f
        val cy = height / 2f
        val size = min(width, height) / 2f - dp(2f)
        val arm = dp(14f)

        path.reset()
        // Top-left corner
        path.moveTo(cx - size, cy - size + arm)
        path.lineTo(cx - size, cy - size)
        path.lineTo(cx - size + arm, cy - size)

        // Top-right corner
        path.moveTo(cx + size - arm, cy - size)
        path.lineTo(cx + size, cy - size)
        path.lineTo(cx + size, cy - size + arm)

        // Bottom-left corner
        path.moveTo(cx - size, cy + size - arm)
        path.lineTo(cx - size, cy + size)
        path.lineTo(cx - size + arm, cy + size)

        // Bottom-right corner
        path.moveTo(cx + size - arm, cy + size)
        path.lineTo(cx + size, cy + size)
        path.lineTo(cx + size, cy + size - arm)

        canvas.drawPath(path, bracketPaint)
        // Center crosshair / dot
        canvas.drawCircle(cx, cy, dp(2.5f), dotPaint)
    }
}

/** Camera permission helpers with "blocked" (don't ask again) detection. */
object CameraPermission {
    private var blocked = false

    fun granted(a: Activity) = a.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
    fun isBlocked() = blocked

    fun request(a: Activity) = a.requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)

    /** Call from onRequestPermissionsResult; returns true when granted. */
    fun onResult(a: Activity, grantResults: IntArray): Boolean {
        val ok = grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED
        blocked = !ok && !a.shouldShowRequestPermissionRationale(Manifest.permission.CAMERA)
        return ok
    }

    /** Re-asks, or opens app settings once the system will no longer show the dialog. */
    fun retry(a: Activity) {
        if (blocked) {
            a.startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", a.packageName, null)))
        } else request(a)
    }

    fun retryLabel() = if (blocked) "Open settings" else "Allow camera"
}
