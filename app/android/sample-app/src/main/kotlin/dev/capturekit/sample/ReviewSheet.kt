package dev.capturekit.sample

import android.app.Activity
import android.app.Dialog
import android.graphics.Color
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import dev.capturekit.capture.CoreEvent
import dev.capturekit.guidance.ObjectSummary
import dev.capturekit.guidance.ScanStateSnapshot
import dev.capturekit.sample.UiKit.dp
import kotlin.math.max

/**
 * Review-before-finish sheet. Shown after the core is paused and before finalize: the same parts checklist as the
 * live HUD ([ChecklistView]) plus the weak walls, with "Scan the gaps" (resume the same bundle) and "Finish anyway".
 * Wall names are "Wall N": N = the core's stable wall id (overlay v3, same names as the voice answers) or, for an older
 * core, the 1-based position in the last snapshot's surfaces. Only evidenced walls count (see
 * [ScanStateSnapshot.evidencedWalls]): hypothesised outline sides, plane-less inferred walls and walls entirely hidden
 * behind furniture are never called weak, so the sheet can always clear.
 */
object ReviewSheet {
    /** Walls below this percentage are called weak (guess until Gate 0 data; the scanning band starts at 20). */
    const val WEAK_PCT = 50

    /** Wall numbers (core ids on a v3 core) whose coverage is below [threshold]. */
    fun weakWalls(o: CoreEvent.Overlay?, threshold: Int = WEAK_PCT): List<Int> {
        if (o == null) return emptyList()
        if (!o.layoutV3) return o.surfaces.withIndex().filter { it.value.pct < threshold }.map { it.index + 1 }
        return ScanStateSnapshot.evidencedWalls(o).filter { it.pct < threshold }.map { it.id }
    }

    /** Nothing of the room captured: no snapshot at all, or no floor and no wall seen. */
    fun nothingFound(o: CoreEvent.Overlay?): Boolean = o == null || (!o.floorFound && o.walls == 0 && o.surfaces.isEmpty())

    /**
     * The sheet is skipped (finish straight away) only when something was captured and no wall is weak. With
     * [guidance] off (ADR 0032) the weak-wall part is gated: only the empty-scan notice remains.
     */
    fun needed(o: CoreEvent.Overlay?, guidance: Boolean): Boolean = nothingFound(o) || (guidance && weakWalls(o).isNotEmpty())

    /**
     * [guidance] off: minimal empty-scan notice (title, one line, the two buttons); no checklist or walls.
     * The "Found" line needs [detector] (ADR 0032), as on the Review screen.
     */
    fun show(act: Activity, o: CoreEvent.Overlay?, guidance: Boolean, detector: Boolean, onGaps: () -> Unit, onFinish: () -> Unit): Dialog {
        val weak = if (guidance) weakWalls(o) else emptyList()
        val empty = nothingFound(o)
        val segs = o?.let { ScanStateSnapshot.evidencedWalls(it) }.orEmpty()
        var done = 0; var scanning = 0
        for (s in segs) if (s.pct >= RoomView.DONE_PCT) done++ else if (s.pct >= ChecklistView.SCANNING_PCT) scanning++
        val pct = Math.round(((o?.completeness ?: 0.0) * 100).coerceIn(0.0, 100.0)).toInt()
        val checklist = ChecklistView(act).apply { set(o?.floorFound ?: false, (o?.ceilingFound ?: false) || (o?.ceilingSeen ?: false), if (o == null) 0 else ChecklistView.foundWalls(o), done, scanning, pct) }

        val dlg = Dialog(act)
        fun text(t: String, sp: Float, bold: Boolean = false) = TextView(act).apply {
            this.text = t; textSize = sp; setTextColor(Color.WHITE)
            if (bold) typeface = Typeface.DEFAULT_BOLD
        }
        val gapsText = if (empty) "Nothing of the room was captured yet." else if (weak.isEmpty()) "All evidenced walls meet coverage targets." else
            "Needs more scanning: " + weak.joinToString(", ") { "Wall $it" } + "."
        val handle = View(act).apply {
            background = UiKit.rounded(0x4DFFFFFF, act.dp(3).toFloat())
        }

        val found = if (!detector) "" else o?.let { ObjectSummary.text(it.objects, it.openings) }.orEmpty()   // issue #2 D: what was recognised so far
        val root = LinearLayout(act).apply {
            orientation = LinearLayout.VERTICAL
            background = UiKit.card(act, 28)
            setPadding(act.dp(24), act.dp(16), act.dp(24), act.dp(24))

            // Top drag handle
            addView(handle, LinearLayout.LayoutParams(act.dp(40), act.dp(5)).apply {
                gravity = Gravity.CENTER_HORIZONTAL
                bottomMargin = act.dp(16)
            })

            addView(text("Before you finish", 22f, true))
            if (guidance) addView(checklist, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = act.dp(10) })
            addView(text(gapsText, UiKit.TEXT_M).apply {
                setTextColor(if (empty || weak.isNotEmpty()) UiKit.AMBER else 0xFFB0BEC5.toInt())
                contentDescription = if (guidance) "$gapsText ${checklist.description}" else gapsText
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = act.dp(10) })
            if (found.isNotEmpty()) addView(text("Found: $found.", UiKit.TEXT_M).apply {
                setTextColor(0xFFCFD8DC.toInt())
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = act.dp(6) })

            val btns = LinearLayout(act).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.END or Gravity.CENTER_VERTICAL
            }
            btns.addView(UiKit.secondaryButton(act, "Finish anyway").apply {
                textSize = 16f
                setOnClickListener { dlg.dismiss(); onFinish() }
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

            btns.addView(UiKit.bigButton(act, if (empty) "Keep scanning" else "Scan the gaps", UiKit.BLUE_ACCENT).apply {
                textSize = 16f
                setOnClickListener { dlg.dismiss(); onGaps() }
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.4f).apply { marginStart = act.dp(12) })

            addView(btns, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = act.dp(20) })
        }
        dlg.window?.setBackgroundDrawableResource(android.R.color.transparent)
        dlg.setContentView(root)
        dlg.setCancelable(false)   // the scan is paused: the only ways out are the two buttons
        dlg.show()
        return dlg
    }
}
