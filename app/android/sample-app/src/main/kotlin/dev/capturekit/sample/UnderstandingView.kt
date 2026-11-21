package dev.capturekit.sample

import android.content.Context
import android.os.SystemClock
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import dev.capturekit.capture.CoreEvent
import kotlin.math.max

/**
 * Parts checklist: "Floor", "Walls: 2 done, 1 scanning", "Ceiling" plus an optional small percentage caption.
 * Walls are counted among the FOUND walls (never "x of 4": there is no room shell yet). Thresholds are guesses
 * until Gate 0 data. Fixed labels only; the rows are summarised by [description] (parts, never a percentage).
 */
class ChecklistView(ctx: Context) : LinearLayout(ctx) {
    private val d = ctx.resources.displayMetrics.density
    private fun row() = TextView(context).apply { textSize = UiKit.TEXT_S; maxLines = 1 }
    private val floorRow = row()
    private val wallsRow = row()
    private val ceilingRow = row()
    private val pctRow = row().apply { setTextColor(DIM); textSize = SMALL }

    /** Plain-language summary for TalkBack; parts only, no percentage. */
    var description = ""
        private set

    init {
        orientation = VERTICAL
        addView(floorRow); addView(wallsRow); addView(ceilingRow)
        addView(pctRow, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT).apply { topMargin = (2 * d).toInt() })
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS
    }

    /** [found] walls seen so far, [done] of them at or above [RoomView.DONE_PCT], [scanning] in the scanning band. [pct] < 0 hides the caption. */
    fun set(floor: Boolean, ceiling: Boolean, found: Int, done: Int, scanning: Int, pct: Int) {
        mark(floorRow, floor, "Floor")
        mark(ceilingRow, ceiling, "Ceiling (optional)")
        val walls = wallsText(found, done, scanning)
        wallsRow.text = walls
        wallsRow.setTextColor(if (found > 0 && done == found) OK else if (found > 0) PROGRESS else DIM)
        setPct(pct)
        description = "Floor ${if (floor) "found" else "not found yet"}. $walls. Ceiling ${if (ceiling) "found" else "not found yet"}."
    }

    /** Updates only the small percentage caption ([pct] < 0 hides it). */
    fun setPct(pct: Int) {
        if (pct < 0) pctRow.visibility = GONE
        else { pctRow.text = "Scanned $pct%"; pctRow.visibility = VISIBLE }
    }

    private fun mark(v: TextView, on: Boolean, label: String) {
        v.text = (if (on) "● " else "○ ") + label
        v.setTextColor(if (on) OK else DIM)
    }

    companion object {
        const val SCANNING_PCT = 20           // scanning band 20-79, done >= RoomView.DONE_PCT (guesses)
        const val SMALL = 11f
        private const val OK = UiKit.GREEN
        private const val PROGRESS = UiKit.CYAN_ACCENT
        private const val DIM = 0xFF8E8E93.toInt()

        /** Layout wall count: `walls` from a v3 core (evidenced layout walls), else the older max(plane count, surfaces). */
        fun foundWalls(o: CoreEvent.Overlay): Int = if (o.layoutV3) o.walls else max(o.walls, o.surfaces.size)

        /** "Walls 4 (2 done, 1 scanning)"; walls below the scanning band count as "just found". */
        fun wallsText(found: Int, done: Int, scanning: Int): String {
            if (found <= 0) return "Walls: none yet"
            val sb = StringBuilder("Walls ").append(found)
            val parts = ArrayList<String>(3)
            if (done > 0) parts.add("$done done")
            if (scanning > 0) parts.add("$scanning scanning")
            val fresh = found - done - scanning
            if (fresh > 0) parts.add("$fresh just found")
            if (parts.isNotEmpty()) sb.append(" (").append(parts.joinToString(", ")).append(')')
            return sb.toString()
        }
    }
}

/**
 * "What the app understands" card for the capture HUD's left-centre zone: the parts checklist and a small caption
 * with the core's reason line (why_text, else the core's long_text). The live mini room was removed from the capture
 * screen (issue #8: the camera mask overlay replaces it); [RoomView] stays on the review screen. All sentences come
 * from the core; only fixed labels live here. Tap the card to hide the caption. Updates are throttled to 4 Hz and must
 * be called on the UI thread.
 * Feedback: a haptic tick when another wall reaches the done threshold; a short green glow plus a confirm
 * haptic when the core reports mode "finish" (the core speaks "Scan complete" itself).
 */
class UnderstandingView(private val ctx: Context) {
    private val d = ctx.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private val checklist = ChecklistView(ctx)
    private val caption = TextView(ctx).apply {
        textSize = UiKit.TEXT_S; setTextColor(UiKit.LABEL_2); maxLines = 2
        ellipsize = android.text.TextUtils.TruncateAt.END; visibility = View.GONE
    }
    private val glassBg = UiKit.card(ctx, 22)
    private val glowBg = UiKit.rounded(GLOW, dp(22).toFloat())
    private val unglow = Runnable { card.background = glassBg }

    /** Left-centre zone card. Hidden until the first snapshot. */
    val card = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        visibility = View.GONE
        isClickable = true
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
        background = glassBg
        setPadding(dp(6), dp(6), dp(6), dp(6))
        addView(checklist, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        addView(caption, LinearLayout.LayoutParams(dp(MAP_W_DP), LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2) })
        setOnClickListener { collapsed = !collapsed; render() }
    }

    private var collapsed = false
    private var last: CoreEvent.Overlay? = null
    private var shown: CoreEvent.Overlay? = null
    private var lastRenderMs = 0L
    private var longText = ""
    private var finishSeen = false
    private var shownFound = -1
    private var shownDone = -1
    private var maxDone = -1
    private var shownScanning = -1
    private var shownFloor = false
    private var shownCeiling = false
    private var shownPct = -1
    private var shownCollapsed = false
    private var shownFoundText = ""

    init {
        // Children are summarised by the card's contentDescription; keep TalkBack from reading them again.
        caption.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    /** The core's long_text for the current instruction; shown as the caption when the core gives no why_text. */
    fun setLongText(t: String) {
        if (t == longText) return
        longText = t
        if (last != null) render()
    }

    /** Newest snapshot (null = not scanning: hide everything). Renders at most every 250 ms (4 Hz). */
    fun update(o: CoreEvent.Overlay?, nowMs: Long = SystemClock.elapsedRealtime()) {
        if (o == null) {
            last = null; shown = null; shownFound = -1; shownDone = -1; maxDone = -1; finishSeen = false
            card.removeCallbacks(unglow); card.background = glassBg
            card.visibility = View.GONE
            return
        }
        last = o
        if (o.mode != "finish") finishSeen = false
        else if (!finishSeen) {
            // Glow only: the core's Finish instruction already plays the "success" haptic.
            finishSeen = true
            card.background = glowBg
            card.removeCallbacks(unglow); card.postDelayed(unglow, GLOW_MS)
        }
        if (o === shown || nowMs - lastRenderMs < 250) return
        lastRenderMs = nowMs
        render()
    }

    private fun render() {
        val o = last ?: return
        shown = o
        val pct = Math.round((o.completeness * 100).coerceIn(0.0, 100.0)).toInt()
        var done = 0; var scanning = 0
        val segs = dev.capturekit.guidance.ScanStateSnapshot.evidencedWalls(o)
        for (i in 0 until segs.size) {
            val p = segs[i].pct
            if (p >= RoomView.DONE_PCT) done++ else if (p >= ChecklistView.SCANNING_PCT) scanning++
        }
        val found = ChecklistView.foundWalls(o)
        val ceilingSeen = o.ceilingFound || o.ceilingSeen
        // Text is rebuilt only when what it says changes (not on every 4 Hz render).
        val foundText = dev.capturekit.guidance.ObjectSummary.text(o.objects, o.openings)
        val textChanged = found != shownFound || done != shownDone || scanning != shownScanning ||
            o.floorFound != shownFloor || ceilingSeen != shownCeiling || foundText != shownFoundText
        if (textChanged) {
            checklist.set(o.floorFound, ceilingSeen, found, done, scanning, pct)
            // Haptic tick only when the done count reaches a new maximum (not on the first snapshot, not on flicker).
            if (maxDone >= 0 && done > maxDone) UiKit.haptic(card, false)
            if (done > maxDone) maxDone = done
        } else if (pct != shownPct) checklist.setPct(pct)
        if (textChanged || collapsed != shownCollapsed) {
            // Parts, never a percentage (the percentage is a small caption, hidden from TalkBack).
            card.contentDescription = "Room parts. ${checklist.description} " +
                (if (foundText.isEmpty()) "" else "Found: $foundText. ") +
                if (collapsed) "Details hidden. Tap to show." else "Tap to hide details."
        }
        card.visibility = View.VISIBLE
        shownFound = found; shownDone = done; shownScanning = scanning
        shownFloor = o.floorFound; shownCeiling = ceilingSeen; shownPct = pct; shownCollapsed = collapsed; shownFoundText = foundText
        val t = o.whyText.ifEmpty { longText }
        if (t.isEmpty() || collapsed) caption.visibility = View.GONE
        else { if (caption.text.toString() != t) caption.text = t; caption.visibility = View.VISIBLE }
    }

    private companion object {
        const val MAP_W_DP = 170   // caption width (was the mini-map width)
        const val GLOW = 0xCC2E7D32.toInt()
        const val GLOW_MS = 1200L
    }
}
