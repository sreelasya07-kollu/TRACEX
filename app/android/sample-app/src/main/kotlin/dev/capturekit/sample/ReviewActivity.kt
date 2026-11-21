package dev.capturekit.sample

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import dev.capturekit.capture.EventParser
import dev.capturekit.guidance.ObjectSummary
import kotlin.math.max
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import dev.capturekit.nativebridge.Status
import dev.capturekit.sample.upload.UploadGate
import dev.capturekit.upload.UploadState
import dev.capturekit.upload.UploadStatus
import dev.capturekit.upload.UploadWorker
import dev.capturekit.verdict.ScanInfo
import dev.capturekit.verdict.VerdictText
import java.io.File

/**
 * Review screen after Finish: save status, the validator verdict the core wrote into manifest.json
 * (PASS / WARN / FAIL) in plain language (assets/verdict_messages.json), keyframes, size, duration and the bundle
 * path. Field flavour: queues the upload when the user allowed it and the verdict is not FAIL, and shows its state.
 * Standard flavour has no INTERNET permission: nothing is queued and no upload state is shown.
 */
class ReviewActivity : Activity() {
    data class Parsed(val verdict: String?, val keyframes: Int?, val issues: List<String>, val durationMs: Long? = null)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val dir = File(intent.getStringExtra(EXTRA_DIR) ?: "")
        val status = intent.getIntExtra(EXTRA_STATUS, Status.STATE)
        val elapsed = intent.getLongExtra(EXTRA_ELAPSED_MS, 0L)
        val kfLive = intent.getIntExtra(EXTRA_KEYFRAMES, 0)
        setContentView(UiKit.page(this, TextView(this).apply {
            text = "Checking the scan..."; textSize = 17f; setTextColor(UiKit.LABEL_2); gravity = Gravity.CENTER
        }))
        // Disk reads (manifest, bundle size) stay off the UI thread.
        Thread {
            val text = try { File(dir, "manifest.json").readText() } catch (t: Throwable) { "" }
            val parsed = parseManifest(text)
            val issues = VerdictText.issuesOf(text)
            val vt = ScanUi.verdictText(applicationContext)
            val size = try { UiKit.dirSizeBytes(dir) } catch (t: Throwable) { 0L }
            // Upload only with the user's upload consent and a non-FAIL verdict (field flavour; a no-op in standard).
            if (status == Status.OK && UploadGate.available) try {
                // Only a never-queued scan is queued here; a failed one waits for the explicit Retry in My scans.
                if (UploadWorker.status(applicationContext, dir).state == UploadState.NONE) UploadGate.enqueueIfAllowed(applicationContext, dir, parsed.verdict)
            } catch (t: Throwable) { }
            runOnUiThread { if (!isDestroyed) build(dir, status, elapsed, kfLive, parsed, size, vt, issues) }
        }.start()
    }

    private var uploadChip: TextView? = null

    /** Polls WorkManager off the main thread until the upload reaches a final state or the screen closes. */
    private fun watchUpload(dir: File) {
        Thread {
            while (!isDestroyed) {
                val st = UploadWorker.status(applicationContext, dir)
                runOnUiThread { if (!isDestroyed) uploadChip?.let { ScanUi.paintUpload(it, st) } }
                if (st.state == UploadState.VERIFIED || st.state == UploadState.FAILED || st.state == UploadState.NONE) break
                try { Thread.sleep(2000) } catch (e: InterruptedException) { break }
            }
        }.start()
    }

    private fun build(dir: File, status: Int, elapsed: Long, kfLive: Int, manifest: Parsed, size: Long, vt: VerdictText, issueList: List<VerdictText.Issue>) {
        val verdict = if (status == Status.OK) manifest.verdict else null
        val kf = manifest.keyframes ?: kfLive
        val ok = status == Status.OK
        val ready = ok && verdict != "FAIL"

        // Landscape: two columns; portrait: one column. The whole screen scrolls and the actions sit below the card,
        // so they are reachable in both orientations.
        val landscape = resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
        val left = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val right = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER_VERTICAL }
        val card = LinearLayout(this).apply {
            orientation = if (landscape) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
            background = UiKit.card(this@ReviewActivity, 28)
            setPadding(dp(24), dp(20), dp(24), dp(20))
            if (landscape) {
                addView(left, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { rightMargin = dp(24) })
                addView(right, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            } else {
                addView(left, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(12) })
                addView(right, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            }
        }
        // Feature switches (ADR 0032): room drawing + walls = guidance, "Found" counts = detector.
        val features = Features.get(this)
        // Final room: the last live snapshot (walls inked, floor/ceiling) and the parts checklist, from Intent extras.
        if (features.guidance && intent.hasExtra(EXTRA_SURFACES)) {
            val segs = EventParser.parseSurfaces(intent.getStringExtra(EXTRA_SURFACES) ?: "")
            val floor = intent.getBooleanExtra(EXTRA_FLOOR, false)
            val ceiling = intent.getBooleanExtra(EXTRA_CEILING, false)
            val shell = EventParser.parseShell(intent.getStringExtra(EXTRA_SHELL) ?: "")
            val closed = intent.getBooleanExtra(EXTRA_SHELL_CLOSED, false)
            val spans = EventParser.parseSpans(intent.getStringExtra(EXTRA_SPANS) ?: "")
            val objects = EventParser.parseObjects(intent.getStringExtra(EXTRA_OBJECTS) ?: "")
            val openings = EventParser.parseOpenings(intent.getStringExtra(EXTRA_OPENINGS) ?: "")
            var done = 0; var scanning = 0
            for (w in segs) if (w.pct >= RoomView.DONE_PCT) done++ else if (w.pct >= ChecklistView.SCANNING_PCT) scanning++
            val checklist = ChecklistView(this).apply {
                set(floor, ceiling, max(intent.getIntExtra(EXTRA_WALLS_FOUND, 0), 0), done, scanning, -1)
            }
            val mid = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES
                contentDescription = "Room parts. ${checklist.description}"
                background = UiKit.rounded(UiKit.FILL, dp(18).toFloat())
                setPadding(dp(6), dp(6), dp(6), dp(6))
                if (segs.isNotEmpty() || shell.isNotEmpty()) addView(RoomView(this@ReviewActivity).also { it.setSnapshot(segs, floor, ceiling, shell, closed, spans, objects, openings) }, LinearLayout.LayoutParams(dp(170), dp(130)))
                addView(checklist, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(4) })
            }
            card.addView(mid, 1, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { rightMargin = dp(24); gravity = Gravity.CENTER_VERTICAL })
        }
        left.addView(UiKit.title(this, if (ok) "Scan saved" else "Save failed", 28f), lp(10))
        val (vText, vColor) = when {
            !ok -> "Not saved (status ${UiKit.statusName(status)})" to UiKit.RED
            else -> ScanInfo.badgeText(verdict) to ScanUi.verdictColor(verdict)
        }
        left.addView(UiKit.chip(this).also { UiKit.setChip(it, vText, vColor) }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(16) })
        // Live elapsed time when coming from a scan; otherwise the bundle's own clock span (opened from My scans).
        val durationMs = if (elapsed > 0) elapsed else manifest.durationMs
        if (durationMs != null && durationMs > 0) left.addView(row("Duration", UiKit.mmss(durationMs)), lp(6))
        left.addView(row("Keyframes", kf.toString()), lp(6))
        // What the scan recognised (issue #2 D): objects by class plus doors / windows, from the last room snapshot.
        if (features.detector) {
            val found = ObjectSummary.text(
                EventParser.parseObjects(intent.getStringExtra(EXTRA_OBJECTS) ?: ""),
                EventParser.parseOpenings(intent.getStringExtra(EXTRA_OPENINGS) ?: ""),
            )
            if (found.isNotEmpty()) left.addView(row("Found", found, wrap = true).apply { contentDescription = "Found in the room: $found" }, lp(6))
            else if (intent.getBooleanExtra(EXTRA_OBJECTS_EXPECTED, false)) left.addView(row("Found", "No objects recognised"), lp(6))
        }
        left.addView(row("Size", UiKit.humanSize(size)), lp(6))
        left.addView(row("Folder", dir.name), lp(6))
        // Full path, wrapped (never clipped); selectable so it can be copied.
        left.addView(UiKit.pathText(this, dir.absolutePath, Int.MAX_VALUE).apply { ellipsize = null; setTextIsSelectable(true) }, lp(12))
        // Plain-language result: headline, then up to three notes (title, what happened, what to do), errors first.
        if (ok && verdict != null) {
            right.addView(UiKit.text(this, vt.headline(verdict), 17f, UiKit.LABEL, UiKit.MEDIUM), lp(8))
            val notes = vt.describe(issueList)
            for (n in notes.take(3)) right.addView(TextView(this).apply {
                text = UiKit.dotted("${n.title}. ${n.text} ${n.action}", if (n.severity == "ERROR") UiKit.RED else UiKit.AMBER)
                textSize = 15f; setTextColor(UiKit.LABEL_2); setLineSpacing(0f, 1.15f)
                contentDescription = "${if (n.severity == "ERROR") "Problem" else "Note"}: ${n.title}. ${n.text} ${n.action}"
            }, lp(4))
            if (notes.size > 3) right.addView(TextView(this).apply { text = "and ${notes.size - 3} more"; textSize = 14f; setTextColor(UiKit.LABEL_3) }, lp(4))
        }
        right.addView(TextView(this).apply {
            text = when {
                ready && UploadGate.available -> "The scan is stored on this phone."
                ready -> "The scan is stored on this phone."
                ok -> "Not ready: the check failed. Scan again, moving slowly and covering the whole room."
                else -> "Something went wrong while saving. Scan again."
            }
            textSize = 15f; setTextColor(UiKit.LABEL_2)
        }, lp(8))
        if (ready && UploadGate.available) {
            val chip = UiKit.chip(this).also { uploadChip = it; it.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_YES; it.setPadding(dp(12), dp(6), dp(12), dp(6)) }
            ScanUi.paintUpload(chip, UploadStatus(UploadState.NONE))
            right.addView(chip, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(16) })
            watchUpload(dir)
        } else right.addView(View(this), lp(8))

        val newScan = UiKit.bigButton(this, "New scan").apply {
            setOnClickListener { startActivity(Intent(this@ReviewActivity, CaptureActivity::class.java)); finish() }
        }
        val home = UiKit.secondaryButton(this, "Done").apply { setOnClickListener { finish() } }
        val scans = UiKit.secondaryButton(this, "My scans").apply {
            contentDescription = "Open My scans"
            setOnClickListener { startActivity(Intent(this@ReviewActivity, ScansActivity::class.java)); finish() }
        }
        // Actions live below the card inside the scrolling content (visible/reachable in both orientations).
        val actions = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(scans, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { rightMargin = dp(12) })
            addView(home, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { rightMargin = dp(12) })
            addView(newScan, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.3f))
        }

        val scroll = ScrollView(this).apply {
            isFillViewport = true
            isVerticalScrollBarEnabled = false
            addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER_VERTICAL
                addView(UiKit.circularIconButton(context, R.drawable.ic_close, "Close", 44).apply { setOnClickListener { finish() } },
                    LinearLayout.LayoutParams(dp(44), dp(44)).apply { bottomMargin = dp(12) })
                addView(card, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(12) })
                addView(actions, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            }, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            UiKit.applyInsets(this, 16)
        }
        setContentView(UiKit.page(this, scroll))
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun lp(bottom: Int) = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(bottom) }

    /** Label + value row; [wrap] lets a long value use several lines (else one line, middle-ellipsised). */
    private fun row(label: String, value: String, wrap: Boolean = false) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        addView(TextView(context).apply { text = label; textSize = 15f; setTextColor(UiKit.LABEL_3) }, LinearLayout.LayoutParams(dp(100), ViewGroup.LayoutParams.WRAP_CONTENT))
        addView(TextView(context).apply {
            text = value; textSize = 15f; typeface = UiKit.MEDIUM; setTextColor(UiKit.LABEL)
            if (!wrap) { maxLines = 1; ellipsize = android.text.TextUtils.TruncateAt.MIDDLE }
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) UiKit.immersive(this)
    }

    companion object {
        const val EXTRA_DIR = "dir"
        const val EXTRA_STATUS = "status"
        const val EXTRA_ELAPSED_MS = "elapsed_ms"
        const val EXTRA_KEYFRAMES = "keyframes"
        /** Last overlay snapshot from the scan: surfaces string ("x0,z0,x1,z1,pct;...") + flags. Absent when no overlay arrived. */
        const val EXTRA_SURFACES = "room_surfaces"
        const val EXTRA_FLOOR = "room_floor"
        const val EXTRA_CEILING = "room_ceiling"
        const val EXTRA_WALLS_FOUND = "room_walls_found"
        /** Layout outline and details of the last overlay (real-room-fix T3): same string formats as the overlay v3 keys. */
        const val EXTRA_SHELL = "room_shell"
        const val EXTRA_SHELL_CLOSED = "room_shell_closed"
        const val EXTRA_SPANS = "room_spans"
        const val EXTRA_OBJECTS = "room_objects"
        const val EXTRA_OPENINGS = "room_openings"
        /** True when the scan lifted detections to objects (detector + understanding on): "No objects recognised" is then meaningful. */
        const val EXTRA_OBJECTS_EXPECTED = "room_objects_expected"

        /** manifest.json: validation.verdict, counts.keyframes and WARN/ERROR messages from validation.issues (INFO hidden). */
        fun parseManifest(text: String): Parsed = try {
            val root = org.json.JSONObject(text)
            val validation = root.optJSONObject("validation")
            val verdict = validation?.optString("verdict", "")?.takeIf { it == "PASS" || it == "WARN" || it == "FAIL" }
            val kf = root.optJSONObject("counts")?.let { if (it.has("keyframes")) it.optInt("keyframes", -1).takeIf { n -> n >= 0 } else null }
            val arr = validation?.optJSONArray("issues")
            val issues = ArrayList<String>()
            if (arr != null) for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val sev = o.optString("severity", "")
                val msg = o.optString("message", "")
                if ((sev == "WARN" || sev == "ERROR") && msg.isNotEmpty()) issues.add(msg)
            }
            val clock = root.optJSONObject("clock")
            val dur = if (clock != null && clock.has("t_start_ns") && clock.has("t_end_ns"))
                ((clock.optLong("t_end_ns") - clock.optLong("t_start_ns")) / 1_000_000L).takeIf { it > 0 } else null
            Parsed(verdict, kf, issues, dur)
        } catch (t: Throwable) { Parsed(null, null, emptyList()) }   // missing/corrupt manifest: show "not available"
    }
}
