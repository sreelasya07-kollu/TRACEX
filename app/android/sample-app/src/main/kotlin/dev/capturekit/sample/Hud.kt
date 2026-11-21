package dev.capturekit.sample

import android.animation.ObjectAnimator
import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import dev.capturekit.capture.FeatureFlags
import dev.capturekit.guidance.StatusPill

/**
 * Shared capture HUD over the camera preview:
 *  - Top bar: Dynamic Now Bar status pill (tracking + quality + thermal) left; REC time + trial + voice right
 *  - Viewfinder reticle: Professional spatial corner brackets [ ] with center targeting dot
 *  - Middle row: Room understanding card left-centre (parts checklist + core caption); debug messages beside
 *  - Floating guidance: Now Bar coaching capsule anchored above camera deck with smooth fade transitions
 *  - Camera deck: Authentic camera shutter button (outer ring + solid core / stop square), flanked by circular glass controls
 * Long-press the status pill to toggle the ingest debug line. Clamped by system bar / cutout insets.
 * [features] (ADR 0032): a switched-off feature's view is never added / stays GONE and gets no click listener
 * (room card = guidance, boxes chip = detector, masks chip = segmentation, mic chip = voiceCommands, recalibration
 * card = recalibrateCard). One camera-overlay chip (issue #8): while the masks chip is shown it replaces the boxes chip.
 */
class Hud(private val ctx: Activity, private val features: FeatureFlags.Flags = FeatureFlags.Flags.DEFAULTS) {
    private val d = ctx.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private val pill = UiKit.chip(ctx).apply { textSize = UiKit.TEXT_M }
    private val understanding = UnderstandingView(ctx)
    private val detail = glassText(UiKit.TEXT_M).apply { visibility = View.GONE }
    /** Voice-command feedback ("Paused.", answers, notes): its own line so it never overwrites [detail]. */
    private val voiceLine = glassText(UiKit.TEXT_M).apply { visibility = View.GONE; maxLines = 3; ellipsize = android.text.TextUtils.TruncateAt.END }
    /** Ingest counters line (trk/frm/kf/pts/pl); hidden until the status pill is long-pressed. */
    private val debug = glassText(UiKit.TEXT_S).apply { visibility = View.GONE; typeface = Typeface.MONOSPACE }
    var debugOn = false
        private set
    private val result = glassText(UiKit.TEXT_M).apply { visibility = View.GONE; ellipsize = android.text.TextUtils.TruncateAt.MIDDLE; maxLines = 4 }
    private val recDot = View(ctx).apply { background = UiKit.rounded(UiKit.RED, dp(5).toFloat()) }
    private val recText = TextView(ctx).apply { textSize = 14f; setTextColor(UiKit.LABEL); typeface = UiKit.MEDIUM; fontFeatureSettings = "tnum" }
    private val recRow = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        visibility = View.GONE
        background = UiKit.nowBar(ctx, 0xE6141824.toInt())
        setPadding(dp(14), dp(6), dp(16), dp(6))
        addView(recDot, LinearLayout.LayoutParams(dp(10), dp(10)).apply { rightMargin = dp(8) })
        addView(recText)
    }
    private val voice = UiKit.chip(ctx).apply { visibility = View.GONE; textSize = UiKit.TEXT_S }
    /** Camera-feed detector boxes on/off (issue #2 C). Hidden unless the detector runs in this scan. */
    private val boxes = UiKit.chip(ctx).apply {
        visibility = View.GONE; textSize = UiKit.TEXT_S; gravity = Gravity.CENTER
        minimumHeight = dp(48)   // 48 dp touch target
    }
    /** Camera-feed segmentation masks on/off (issue #8), in the boxes chip's place. Hidden unless segmentation runs in this scan. */
    private val masks = UiKit.chip(ctx).apply {
        visibility = View.GONE; textSize = UiKit.TEXT_S; gravity = Gravity.CENTER
        minimumHeight = dp(48)   // 48 dp touch target
    }
    private var boxesWanted: Boolean? = null   // last setBoxes value, re-applied when the masks chip goes away
    /** Push-to-talk chip (top-right zone, left of the voice toggle). Hidden unless voice commands are available. */
    private val mic = UiKit.chip(ctx).apply {
        visibility = View.GONE; textSize = UiKit.TEXT_S; gravity = Gravity.CENTER
        minimumHeight = dp(44); minimumWidth = dp(44)
    }
    /** Field research chip (trial setup); hidden unless [setTrial] is given a label (field flavour, not recording). */
    private val trial = UiKit.chip(ctx).apply {
        visibility = View.GONE; textSize = UiKit.TEXT_S; gravity = Gravity.CENTER
        minimumHeight = dp(44)
    }
    private val reticle = ReticleView(ctx).apply { visibility = View.GONE }

    private val coaching = TextView(ctx).apply {
        textSize = 15f
        setTextColor(UiKit.LABEL)
        typeface = UiKit.MEDIUM
        gravity = Gravity.CENTER
        background = UiKit.nowBar(ctx, 0xF2121624.toInt())
        setPadding(dp(22), dp(10), dp(22), dp(10))
        maxLines = 2
    }
    /** Issue #1: centred "Recalibrating" card with the circling-phone animation (declared before [root], which adds it). */
    private val recalibrate = RecalibrateOverlay(ctx)
    private val bannerText = TextView(ctx)
    private val bannerButton = UiKit.bigButton(ctx, "Retry", 0x33FFFFFF).apply { textSize = UiKit.TEXT_M; minimumHeight = 0; minHeight = 0 }
    private val bannerRow = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        visibility = View.GONE
        addView(bannerText, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        addView(bannerButton, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { leftMargin = dp(12) })
    }

    /** Authentic camera shutter button in the center of the control deck. */
    val button: Button = ShutterButton(ctx).apply {
        minimumWidth = dp(76); minWidth = dp(76)
        minimumHeight = dp(76); minHeight = dp(76)
    }
    /** Circular glass pause/resume button flanking the shutter. */
    val pauseButton: Button = UiKit.circularButton(ctx, "⏸", 50).apply {
        visibility = View.GONE
    }
    /** Leaves the capture screen before recording starts (hidden while recording: Finish is the way out). */
    val closeButton: View = UiKit.circularIconButton(ctx, R.drawable.ic_close, "Close", 44).apply {
        setOnClickListener { ctx.finish() }
    }

    private val pulse = ObjectAnimator.ofFloat(recDot, View.ALPHA, 1f, 0.2f).apply {
        duration = 600; repeatMode = ObjectAnimator.REVERSE; repeatCount = ObjectAnimator.INFINITE
    }
    private var coachText = ""
    private var bannerKey: String? = null
    private var paused = false
    private var recSec = -1L
    private var recPaused = false
    private var recCounter = ""
    private var everTracked = false   // "Starting..." only until the camera first tracks

    private val longSide = maxOf(ctx.resources.displayMetrics.widthPixels, ctx.resources.displayMetrics.heightPixels)
    init {
        pill.maxWidth = (longSide * 0.36).toInt()
        val mid = (longSide * 0.55).toInt()
        coaching.maxWidth = (longSide * 0.7).toInt(); detail.maxWidth = mid; voiceLine.maxWidth = mid; result.maxWidth = mid; debug.maxWidth = mid
        voice.setOnClickListener { onVoiceToggle() }
        if (features.detector) boxes.setOnClickListener { onBoxesToggle() }
        if (features.segmentation) masks.setOnClickListener { onMasksToggle() }
        if (features.voiceCommands) mic.setOnClickListener { onMicClick() }
        trial.setOnClickListener { onTrialClick() }
        pill.setOnLongClickListener { debugOn = !debugOn; if (!debugOn) debug.visibility = View.GONE; true }
        UiKit.setChip(pill, "● " + StatusPill.STARTING.text, StatusPill.STARTING.tone.argb)
    }

    private fun glassText(size: Float) = TextView(ctx).apply {
        textSize = size; setTextColor(UiKit.LABEL); background = UiKit.glass(ctx, 16)
        setPadding(dp(12), dp(6), dp(12), dp(6))
    }

    /** No-op while the debug line is toggled off (long-press the status pill to show it). */
    fun setDebug(text: String) {
        if (!debugOn) return
        debug.visibility = View.VISIBLE
        if (debug.text.toString() != text) debug.text = text
    }

    /** Set to handle taps on the voice chip. */
    var onVoiceToggle: () -> Unit = {}

    val root = FrameLayout(ctx).apply {
        val wrap = ViewGroup.LayoutParams.WRAP_CONTENT
        val match = ViewGroup.LayoutParams.MATCH_PARENT
        val right = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END or Gravity.CENTER_VERTICAL
            addView(recRow, LinearLayout.LayoutParams(wrap, wrap).apply { rightMargin = dp(8) })
            addView(trial, LinearLayout.LayoutParams(wrap, wrap).apply { rightMargin = dp(8) })
            addView(mic, LinearLayout.LayoutParams(wrap, wrap).apply { rightMargin = dp(8) })
            addView(boxes, LinearLayout.LayoutParams(wrap, wrap).apply { rightMargin = dp(8) })
            addView(masks, LinearLayout.LayoutParams(wrap, wrap).apply { rightMargin = dp(8) })
            addView(voice)
        }
        val topBar = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(closeButton, LinearLayout.LayoutParams(dp(44), dp(44)).apply { rightMargin = dp(10) })
            addView(pill, LinearLayout.LayoutParams(wrap, wrap))
            addView(View(ctx), LinearLayout.LayoutParams(0, 0, 1f))
            addView(right, LinearLayout.LayoutParams(wrap, wrap))
        }
        val msgs = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            addView(detail, lp(bottom = 6)); addView(voiceLine, lp(bottom = 6)); addView(debug, lp(bottom = 6)); addView(result)
        }
        val middle = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            if (features.guidance) addView(understanding.card, LinearLayout.LayoutParams(wrap, wrap).apply { gravity = Gravity.CENTER_VERTICAL })
            addView(msgs, LinearLayout.LayoutParams(0, wrap, 1f).apply { leftMargin = dp(8) })
        }

        // Camera shutter deck: symmetrical camera controls (Pause | Shutter | Spacer/Voice)
        val shutterDeck = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            // Left flank: Circular glass Pause button
            addView(pauseButton, LinearLayout.LayoutParams(dp(50), dp(50)).apply { rightMargin = dp(28) })
            // Center: Authentic camera shutter
            addView(button, LinearLayout.LayoutParams(dp(76), dp(76)))
            // Right flank: Balanced spacer (preserves dead-center alignment)
            addView(View(ctx), LinearLayout.LayoutParams(dp(50), dp(50)).apply { leftMargin = dp(28) })
        }

        val column = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            addView(topBar, LinearLayout.LayoutParams(match, wrap))
            addView(bannerRow, LinearLayout.LayoutParams(match, wrap).apply { topMargin = dp(6) })
            addView(middle, LinearLayout.LayoutParams(match, 0, 1f).apply { topMargin = dp(6); bottomMargin = dp(6) })
            addView(coaching, LinearLayout.LayoutParams(wrap, wrap).apply { gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(14) })
            addView(shutterDeck, LinearLayout.LayoutParams(match, wrap).apply { bottomMargin = dp(4) })
        }

        addView(reticle, FrameLayout.LayoutParams(dp(84), dp(84), Gravity.CENTER))
        addView(column, FrameLayout.LayoutParams(match, match))
        if (features.recalibrateCard) addView(recalibrate.card, FrameLayout.LayoutParams(wrap, wrap, Gravity.CENTER))   // above the column, never clickable
        UiKit.applyInsets(this)
    }

    private fun lp(bottom: Int) = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(bottom) }

    /**
     * The single status pill. [reasons] are the latest core frame-quality reason codes, [thermalLevel] the governor
     * policy level. [live] false = no camera frame recently (paused/stalled): neutral pill, never a tracking verdict.
     */
    fun setStatus(state: String, failure: String, reasons: List<String>, thermalLevel: Int, live: Boolean = true) {
        if (!live) { UiKit.setChip(pill, "● Camera paused", StatusPill.Tone.NEUTRAL.argb); return }
        if (state == "TRACKING") everTracked = true
        val p = StatusPill.statusPill(state, failure, reasons, thermalLevel, everTracked)
        UiKit.setChip(pill, "● " + p.text, p.tone.argb)
    }

    /** Tracking-only pill (screens without core quality/thermal, e.g. the raw recorder). */
    fun setTracking(state: String, failure: String, live: Boolean = true) = setStatus(state, failure, emptyList(), 0, live)

    /** Latest core overlay snapshot (null hides it). Rendered at most 4 Hz; UI thread only. */
    fun setUnderstanding(o: dev.capturekit.capture.CoreEvent.Overlay?) { if (features.guidance) understanding.update(o) }

    /** Voice-command feedback line; null hides it. Independent of [setDetail]. */
    fun setVoiceLine(text: String?) {
        voiceLine.visibility = if (text == null) View.GONE else View.VISIBLE
        if (text != null && voiceLine.text.toString() != text) voiceLine.text = text
    }

    fun setDetail(text: String?) {
        detail.visibility = if (text == null) View.GONE else View.VISIBLE
        if (text != null) detail.text = text
    }

    /** Fades to the new coaching sentence; [sub] (core long_text) goes to the room card caption. No-op when unchanged. */
    fun setCoaching(text: String, sub: String = "") {
        if (features.guidance) understanding.setLongText(sub)
        if (text == coachText) return
        coachText = text
        coaching.animate().cancel()
        coaching.animate().alpha(0f).setDuration(150).withEndAction {
            coaching.text = text
            coaching.animate().alpha(1f).setDuration(150).start()
        }.start()
    }

    fun setRecording(on: Boolean) {
        reticle.visibility = if (on) View.VISIBLE else View.GONE
        recRow.visibility = if (on) View.VISIBLE else View.GONE
        if (on) { if (!pulse.isStarted) pulse.start() } else { pulse.cancel(); recDot.alpha = 1f; paused = false }
        (button as? ShutterButton)?.isRecording = on
        button.text = if (on) "Finish Scan" else "Start Scan"
        pauseButton.visibility = if (on && pauseEnabled) View.VISIBLE else View.GONE
        closeButton.visibility = if (on) View.GONE else View.VISIBLE
        if (!on) pauseButton.text = "⏸"
        recSec = -1
    }

    /** Pause is offered only where a screen supports it (guided scan). */
    var pauseEnabled = false

    fun setPaused(on: Boolean) {
        if (on == paused) return
        paused = on
        pauseButton.text = if (on) "▶" else "⏸"
        if (on) { pulse.cancel(); recDot.alpha = 0.3f } else if (!pulse.isStarted) pulse.start()
    }

    /** REC clock; text is rebuilt only when the shown second, pause state or [counter] changes. */
    fun setRecInfo(elapsedMs: Long, counter: String = "") {
        val sec = elapsedMs / 1000
        if (sec == recSec && paused == recPaused && counter == recCounter) return
        recSec = sec; recPaused = paused; recCounter = counter
        recText.text = (if (paused) "PAUSED  " else "REC  ") + UiKit.mmss(elapsedMs) + if (counter.isEmpty()) "" else "  •  $counter"
    }

    fun setVoice(on: Boolean?) {   // null hides the chip
        if (on == null) { voice.visibility = View.GONE; return }
        voice.visibility = View.VISIBLE
        UiKit.setChip(voice, if (on) "Voice on" else "Voice off", if (on) UiKit.GREEN else 0xFF546E7A.toInt())
        voice.contentDescription = if (on) "Voice guidance on. Tap to turn off." else "Voice guidance off. Tap to turn on."
    }

    /** Set to handle taps on the "Boxes" chip. */
    var onBoxesToggle: () -> Unit = {}

    /** Camera-feed boxes chip; null hides it (detector not running, not recording, or the unguided trial arm). */
    fun setBoxes(on: Boolean?) {
        boxesWanted = on
        if (on == null || !features.detector || masks.visibility == View.VISIBLE) { boxes.visibility = View.GONE; return }
        boxes.visibility = View.VISIBLE
        UiKit.setChip(boxes, if (on) "Boxes on" else "Boxes off", if (on) UiKit.GREEN else 0xFF546E7A.toInt())
        boxes.contentDescription = if (on) "Object boxes on the camera view are shown. Tap to hide them." else "Object boxes on the camera view are hidden. Tap to show them."
    }

    /** Set to handle taps on the "Masks" chip. */
    var onMasksToggle: () -> Unit = {}

    /**
     * Camera-feed segmentation masks chip (issue #8); null hides it (segmentation not running, not recording, or the
     * unguided trial arm). While shown it replaces the boxes chip (one camera-overlay control); hiding it brings the
     * boxes chip back as last set.
     */
    fun setMasks(on: Boolean?) {
        if (on == null || !features.segmentation) {
            val wasShown = masks.visibility == View.VISIBLE
            masks.visibility = View.GONE
            if (wasShown) setBoxes(boxesWanted)
            return
        }
        masks.visibility = View.VISIBLE
        boxes.visibility = View.GONE
        UiKit.setChip(masks, if (on) "Masks on" else "Masks off", if (on) UiKit.GREEN else 0xFF546E7A.toInt())
        masks.contentDescription = if (on) "Object and surface masks on the camera view are shown. Tap to hide them." else "Object and surface masks on the camera view are hidden. Tap to show them."
    }

    /** Set to handle taps on the trial chip (field flavour: opens the trial setup). */
    var onTrialClick: () -> Unit = {}

    /** Trial chip text; null hides it (standard flavour, or while recording). */
    fun setTrial(label: String?) {
        if (label == null) { trial.visibility = View.GONE; return }
        trial.visibility = View.VISIBLE
        UiKit.setChip(trial, label, 0xFF6A1B9A.toInt())
        trial.contentDescription = "Research trial: $label. Tap to change participant or guided arm."
    }

    /** Set to handle taps on the mic chip (push-to-talk). */
    var onMicClick: () -> Unit = {}

    /**
     * Push-to-talk chip. null hides it (no voice commands here). Listening is unmistakable: red chip, "Listening"
     * text and a spoken-by-TalkBack description; idle is the quiet HUD blue-grey.
     */
    fun setMic(state: MicState?) {
        if (state == null || !features.voiceCommands) { mic.visibility = View.GONE; return }
        mic.visibility = View.VISIBLE
        val (text, color, desc) = when (state) {
            MicState.IDLE -> Triple("Mic", 0xFF455A64.toInt(), "Voice commands. Tap, then say pause, resume, repeat, what is missing, finish, louder or quieter.")
            MicState.LISTENING -> Triple("Listening...", UiKit.RED, "Listening for a command. Tap to cancel.")
            MicState.THINKING -> Triple("Working...", UiKit.AMBER, "Understanding the command.")
            MicState.NEEDS_PERMISSION -> Triple("Mic off", 0xFF546E7A.toInt(), "Voice commands need microphone permission. Tap to learn more.")
        }
        UiKit.setChip(mic, text, color)
        if (mic.contentDescription != desc) mic.contentDescription = desc
        mic.isEnabled = state != MicState.THINKING
    }

    /** Full-width strip under the top bar; reserved for errors and blocking conditions. */
    fun setBanner(msg: String?, kind: UiKit.Kind = UiKit.Kind.WARN, retryLabel: String? = null, retry: (() -> Unit)? = null) {
        if (msg == null) { bannerRow.visibility = View.GONE; bannerKey = null; return }
        if (retry != null) bannerButton.setOnClickListener { retry() }
        val key = msg + "|" + kind + "|" + retryLabel + "|" + (retry != null)
        if (key == bannerKey && bannerRow.visibility == View.VISIBLE) return   // unchanged: no new drawables
        bannerKey = key
        UiKit.applyBanner(bannerText, msg, kind)
        bannerText.textSize = UiKit.TEXT_M
        bannerRow.background = UiKit.rounded(kind.color, dp(UiKit.RADIUS_DP).toFloat())
        bannerText.background = null
        bannerButton.visibility = if (retry != null) View.VISIBLE else View.GONE
        if (retry != null) bannerButton.text = retryLabel ?: "Retry"
        bannerRow.visibility = View.VISIBLE
    }

    fun setResult(msg: String?, kind: UiKit.Kind = UiKit.Kind.OK) {
        if (msg == null) { result.visibility = View.GONE; return }
        result.text = msg
        result.background = UiKit.rounded(kind.color, dp(UiKit.RADIUS_DP).toFloat())
        result.visibility = View.VISIBLE
    }

    fun haptic(confirm: Boolean) = UiKit.haptic(button, confirm)
    fun hapticFor(haptic: String, priority: Int) = UiKit.hapticFor(button, haptic, priority)

    /**
     * Issue #1 / ADR 0031: the recalibration card. [text] = the core's sentence ([dev.capturekit.guidance.Recalibration.prompt]);
     * null hides it. Shown while the core recovers tracking (keyframe capture paused); dismissed when tracking is back.
     */
    fun setRecalibrate(text: String?) { if (features.recalibrateCard) recalibrate.set(text) }

    fun release() { pulse.cancel(); coaching.animate().cancel(); recalibrate.release() }
}
