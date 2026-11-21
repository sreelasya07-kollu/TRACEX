package dev.capturekit.sample

import android.app.Activity
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Bundle
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import dev.capturekit.capture.ResearchReport
import dev.capturekit.consent.ConsentStore
import dev.capturekit.nativebridge.Status
import dev.capturekit.sample.upload.UploadGate
import dev.capturekit.upload.UploadPlan
import dev.capturekit.upload.UploadPolicy
import dev.capturekit.upload.UploadSettings
import dev.capturekit.upload.UploadState
import dev.capturekit.upload.UploadStatus
import dev.capturekit.upload.UploadWorker
import dev.capturekit.verdict.ScanInfo
import java.io.File
import java.util.Calendar
import java.util.Date
import java.util.concurrent.ConcurrentHashMap

/**
 * "My scans": every bundle on this phone, newest first, with date, room label (field trials), the validator verdict
 * as a coloured badge with words, and (field flavour only) the upload state with progress, Upload / Retry actions and
 * the "Wi-Fi only" switch. All disk and WorkManager reads run on a worker thread; the list refreshes every 2 s while
 * an upload is active. The standard flavour has no INTERNET permission, so it shows no upload UI at all.
 */
class ScansActivity : Activity() {
    private data class Row(val dir: File, val timeMs: Long, val room: String?, val verdict: String?, val size: Long, val upload: UploadStatus, val thumb: Bitmap?)

    private lateinit var list: LinearLayout
    private var wifiOnly = true
    private var generation = 0
    private val thumbs = ConcurrentHashMap<String, Bitmap>()
    @Volatile private var visible = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        wifiOnly = UploadSettings.unmeteredOnly(this)
        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        val header = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(UiKit.circularIconButton(context, R.drawable.ic_arrow_back, "Back to home", 44).apply { setOnClickListener { finish() } },
                LinearLayout.LayoutParams(dp(44), dp(44)).apply { bottomMargin = dp(16) })
            addView(UiKit.largeTitle(context, "My scans"), lp(0))
        }
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(header, lp(20))
        if (UploadGate.available) root.addView(settingsPanel(), lp(16))
        root.addView(ScrollView(this).apply { isFillViewport = true; isVerticalScrollBarEnabled = false; addView(list) }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        UiKit.applyInsets(root, 20)
        setContentView(UiKit.page(this, root))
        showMessage("Loading your scans...")
    }

    override fun onResume() {
        super.onResume()
        visible = true
        startLoop()
    }

    override fun onPause() {
        visible = false
        super.onPause()
    }

    /** Re-reads everything on a worker thread; keeps polling every 2 s while any upload is active and the screen is visible. */
    private fun startLoop() {
        val gen = ++generation
        Thread {
            while (visible && gen == generation) {
                val rows = loadRows()
                runOnUiThread { if (!isDestroyed && gen == generation) render(rows) }
                val active = rows.any { it.upload.state == UploadState.WAITING || it.upload.state == UploadState.UPLOADING || it.upload.state == UploadState.RETRYING }
                if (!active) break
                try { Thread.sleep(2000) } catch (e: InterruptedException) { break }
            }
        }.start()
    }

    private fun loadRows(): List<Row> {
        val root = getExternalFilesDir("bundles") ?: return emptyList()
        val dirs = root.listFiles { f -> f.isDirectory } ?: return emptyList()
        val uploads = UploadGate.available
        return dirs.map { d ->
            val manifest = try { File(d, UploadPlan.MANIFEST).readText() } catch (t: Throwable) { "" }
            val research = try { ResearchReport.fileFor(d).takeIf { it.exists() }?.readText() } catch (t: Throwable) { null }
            Row(d, ScanInfo.timeMs(d.name, d.lastModified()), ScanInfo.roomLabel(research), ReviewActivity.parseManifest(manifest).verdict,
                try { UiKit.dirSizeBytes(d) } catch (t: Throwable) { 0L },
                if (uploads) UploadWorker.status(applicationContext, d) else UploadStatus(UploadState.NONE), thumbFor(d))
        }.sortedByDescending { it.timeMs }
    }

    /** Middle keyframe, decoded small once per scan (worker thread). */
    private fun thumbFor(d: File): Bitmap? {
        thumbs[d.absolutePath]?.let { return it }
        return try {
            val jpgs = File(d, "keyframes").listFiles { f -> f.name.endsWith(".jpg", true) || f.name.endsWith(".jpeg", true) }?.sortedBy { it.name }
            val f = jpgs?.getOrNull(jpgs.size / 2) ?: return null
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(f.path, bounds)
            var sample = 1
            while (bounds.outWidth / (sample * 2) >= 240 && bounds.outHeight / (sample * 2) >= 240) sample *= 2
            BitmapFactory.decodeFile(f.path, BitmapFactory.Options().apply { inSampleSize = sample })?.also { thumbs[d.absolutePath] = it }
        } catch (t: Throwable) { null }
    }

    /** "Today, 12:07", "Yesterday, 21:58", "17 Nov, 21:35", "17 Nov 2025" (phone's own time format). */
    private fun friendlyDate(ms: Long): String {
        val now = Calendar.getInstance(); val c = Calendar.getInstance().apply { timeInMillis = ms }
        val time = android.text.format.DateFormat.getTimeFormat(this).format(Date(ms))
        val days = (startOfDay(now) - startOfDay(c)) / 86_400_000L
        return when {
            days == 0L -> "Today, $time"
            days == 1L -> "Yesterday, $time"
            now.get(Calendar.YEAR) == c.get(Calendar.YEAR) -> android.text.format.DateFormat.format("d MMM", c).toString() + ", $time"
            else -> android.text.format.DateFormat.format("d MMM yyyy", c).toString()
        }
    }

    private fun startOfDay(c: Calendar) = (c.clone() as Calendar).apply {
        set(Calendar.HOUR_OF_DAY, 0); set(Calendar.MINUTE, 0); set(Calendar.SECOND, 0); set(Calendar.MILLISECOND, 0)
    }.timeInMillis

    private fun showMessage(msg: String) {
        list.removeAllViews()
        list.addView(TextView(this).apply { text = msg; textSize = 16f; setTextColor(UiKit.LABEL_2); gravity = Gravity.CENTER; setPadding(0, dp(48), 0, 0) }, lp(0))
    }

    private fun render(rows: List<Row>) {
        list.removeAllViews()
        if (rows.isEmpty()) { renderEmpty(); return }
        val consentUpload = UploadGate.available && ConsentStore.get(this).uploadAllowed()
        if (UploadGate.available && !consentUpload) {
            list.addView(UiKit.banner(this, "Uploading is off. You did not allow uploads in the consent screen.", UiKit.Kind.WARN).apply { textSize = 15f }, lp(12))
        }
        list.addView(UiKit.sectionHeader(this, if (rows.size == 1) "1 scan" else "${rows.size} scans"))
        val group = UiKit.group(this)
        for (r in rows) group.addView(card(r, friendlyDate(r.timeMs), consentUpload))
        list.addView(group, lp(24))
    }

    private fun renderEmpty() {
        list.addView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER
            setPadding(dp(16), dp(48), dp(16), dp(16))
            addView(TextView(context).apply {
                text = "No scans yet"; textSize = 22f; typeface = UiKit.BOLD; setTextColor(UiKit.LABEL); gravity = Gravity.CENTER
            }, lp(8))
            addView(TextView(context).apply {
                text = "Scan a room and it will show up here with its check result."
                textSize = 15f; setTextColor(UiKit.LABEL_2); gravity = Gravity.CENTER
            }, lp(24))
            addView(UiKit.bigButton(context, "Start a scan").apply {
                setOnClickListener { startActivity(Intent(this@ScansActivity, CaptureActivity::class.java)); finish() }
            })
        }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
    }

    private fun card(r: Row, date: String, consentUpload: Boolean): View {
        val badge = UiKit.text(this, UiKit.dotted(ScanInfo.badgeText(r.verdict), ScanUi.verdictColor(r.verdict)), 13f, UiKit.LABEL_2)
        val thumb = FrameLayout(this).apply {
            background = UiKit.rounded(UiKit.FILL, dp(12).toFloat())
            clipToOutline = true
            if (r.thumb != null) addView(ImageView(context).apply { setImageBitmap(r.thumb); scaleType = ImageView.ScaleType.CENTER_CROP },
                FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            else Unit
        }
        val info = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(TextView(context).apply {
                text = r.room ?: date
                textSize = 16f; typeface = UiKit.MEDIUM; setTextColor(UiKit.LABEL); maxLines = 1; ellipsize = TextUtils.TruncateAt.END
            })
            addView(TextView(context).apply {
                text = if (r.room != null) "$date  ·  ${UiKit.humanSize(r.size)}" else UiKit.humanSize(r.size)
                textSize = 13.5f; setTextColor(UiKit.LABEL_3); maxLines = 1; ellipsize = TextUtils.TruncateAt.END
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2); bottomMargin = dp(4) })
            addView(badge)
        }
        val top = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            addView(thumb, LinearLayout.LayoutParams(dp(64), dp(64)).apply { rightMargin = dp(14) })
            addView(info, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(UiKit.iconImage(context, R.drawable.ic_chevron_right, UiKit.LABEL_3), LinearLayout.LayoutParams(dp(18), dp(18)))
        }
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14), dp(12), dp(14), dp(12))
            foreground = UiKit.rowRipple()
            addView(top, lp(0))
        }
        var spoken = "Scan $date${r.room?.let { ", $it" } ?: ""}. Check: ${ScanInfo.badgeText(r.verdict)}."
        if (UploadGate.available) {
            val chip = UiKit.chip(this).also { ScanUi.paintUpload(it, r.upload, wifiOnly) }
            col.addView(chip, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(10); bottomMargin = dp(8) })
            if (r.upload.state == UploadState.UPLOADING) col.addView(ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
                max = 100; progress = r.upload.percent.coerceIn(0, 100)
                contentDescription = "Upload progress ${r.upload.percent} percent"
            }, lp(8))
            spoken += " ${ScanUi.uploadText(r.upload, wifiOnly).replace("%", " percent")}."
            val canUpload = consentUpload && UploadPolicy.eligible(true, r.verdict)
            val action = when {
                canUpload && r.upload.state == UploadState.NONE -> "Upload"
                canUpload && r.upload.state == UploadState.FAILED -> "Retry upload"
                else -> null
            }
            if (action != null) col.addView(UiKit.secondaryButton(this, action).apply {
                contentDescription = "$action for the scan from $date"
                setOnClickListener { isEnabled = false; queue(r.dir, r.verdict, replace = false) }
            }, lp(0))
            else if (r.verdict != "PASS" && r.verdict != "WARN" && r.upload.state == UploadState.NONE) col.addView(TextView(this).apply {
                text = if (r.verdict == "FAIL") "A failed scan is never uploaded." else "Not finalized: this scan cannot be uploaded."; textSize = 13.5f; setTextColor(UiKit.LABEL_3)
            }, lp(0).apply { topMargin = dp(8) })
        }
        col.isClickable = true
        col.contentDescription = spoken + " Double tap for details."
        col.setOnClickListener {
            startActivity(Intent(this, ReviewActivity::class.java).putExtra(ReviewActivity.EXTRA_DIR, r.dir.absolutePath).putExtra(ReviewActivity.EXTRA_STATUS, Status.OK))
        }
        return col
    }

    private fun queue(dir: File, verdict: String?, replace: Boolean) {
        Thread {
            try { UploadGate.enqueueIfAllowed(applicationContext, dir, verdict, replace) } catch (t: Throwable) { }
            runOnUiThread { if (!isDestroyed) startLoop() }
        }.start()
    }

    private fun settingsPanel(): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        background = UiKit.card(context, UiKit.RADIUS_DP)
        setPadding(dp(16), dp(10), dp(16), dp(12))
        addView(UiKit.toggle(context, wifiOnly).apply {
            text = "Upload on Wi-Fi only"; textSize = 16f; setTextColor(UiKit.LABEL); typeface = UiKit.MEDIUM
            minHeight = dp(48)
            contentDescription = "Upload on Wi-Fi only. Turn off to also upload on mobile data."
            setOnCheckedChangeListener { _, on ->
                wifiOnly = on
                Thread {
                    UploadSettings.setUnmeteredOnly(applicationContext, on)
                    // Queued uploads keep the old network rule, so restart the ones that have not finished.
                    for (r in loadRows()) if (r.upload.state == UploadState.WAITING || r.upload.state == UploadState.RETRYING || r.upload.state == UploadState.UPLOADING)
                        try { UploadGate.enqueueIfAllowed(applicationContext, r.dir, r.verdict, replace = true) } catch (t: Throwable) { }
                    runOnUiThread { if (!isDestroyed) startLoop() }
                }.start()
            }
        })
        addView(TextView(context).apply {
            text = "Scans are large. Off means mobile data may be used."; textSize = 13f; setTextColor(UiKit.LABEL_3)
        })
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun lp(bottom: Int) = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(bottom) }
}
