package dev.capturekit.sample

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import dev.capturekit.consent.ConsentStore

/**
 * Consent screen. The text (res/raw/consent_v1.txt, a placeholder pending a legal owner) scrolls; the two
 * optional choices (upload, anonymous telemetry) sit at the end of it and both start off. Accept stays
 * disabled until the user has scrolled to the end. Decline just leaves: no consent, no scanning.
 */
class ConsentActivity : Activity() {
    private lateinit var scroll: ScrollView
    private lateinit var accept: Button
    private lateinit var hint: TextView
    private lateinit var upload: Switch
    private lateinit var telemetry: Switch
    private var reachedEnd = false
    private var accepting = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.decorView.setBackgroundColor(UiKit.BG)
        val text = resources.openRawResource(
            resources.getIdentifier(ConsentStore.TEXT_RESOURCE, "raw", packageName).also { check(it != 0) { "missing raw/${ConsentStore.TEXT_RESOURCE}" } }).bufferedReader().use { it.readText() }
        val (head, rest) = text.lines().let { it.first() to it.drop(1).joinToString("\n").trim() }

        val content = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        content.addView(TextView(this).apply {
            this.text = "Before you scan"; textSize = 32f; typeface = UiKit.BOLD; letterSpacing = -0.02f; setTextColor(UiKit.LABEL)
            isFocusable = true
        }, lp(12))
        content.addView(UiKit.banner(this, head.lowercase().replaceFirstChar { it.uppercase() }, UiKit.Kind.WARN), lp(16))
        content.addView(TextView(this).apply {
            this.text = rest; textSize = 16f; setTextColor(UiKit.LABEL_2); setLineSpacing(0f, 1.25f)
        }, lp(24))
        upload = toggle("Allow uploading my finished scans (optional)")
        telemetry = toggle("Allow anonymous crash and usage reports (optional)")
        content.addView(upload, lp(8)); content.addView(telemetry, lp(24))

        scroll = ScrollView(this).apply {
            isFillViewport = false
            contentDescription = "Consent text. Scroll to the end to enable the Accept button."
            addView(content)
            setOnScrollChangeListener { _, _, _, _, _ -> checkEnd() }
            viewTreeObserver.addOnGlobalLayoutListener { checkEnd() }   // text shorter than the screen: nothing to scroll
        }

        hint = TextView(this).apply { textSize = 13f; setTextColor(UiKit.LABEL_3); gravity = android.view.Gravity.CENTER }
        accept = UiKit.bigButton(this, "Accept and continue").apply {
            contentDescription = "Accept and continue. Available after you scroll to the end of the consent text."
            setOnClickListener { onAccept() }
        }
        val decline = UiKit.secondaryButton(this, "Decline").apply {
            contentDescription = "Decline. You will not be able to scan without consent."
            setOnClickListener { setResult(RESULT_CANCELED); finish() }
        }
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(hint, lp(8)); addView(accept, lp(8)); addView(decline, lp(0))
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
            addView(bar, lp(0).apply { topMargin = dp(8) })
            UiKit.applyInsets(this, 20)
        }
        setContentView(UiKit.page(this, root))
        paint()
    }

    private fun checkEnd() {
        if (reachedEnd) return
        if (!scroll.canScrollVertically(1) && scroll.getChildAt(0).height > 0) {
            reachedEnd = true
            paint()
            accept.announceForAccessibility("Accept is now available")
        }
    }

    private fun paint() {
        accept.isEnabled = reachedEnd
        UiKit.recolor(accept, if (reachedEnd) UiKit.PRIMARY else UiKit.GLASS)
        accept.setTextColor(if (reachedEnd) Color.BLACK else UiKit.LABEL_3)
        hint.text = if (reachedEnd) "You can accept now. Both optional choices are off unless you turned them on."
            else "Scroll down and read to the end to enable Accept."
    }

    private fun onAccept() {
        if (!reachedEnd || accepting) return
        accepting = true
        accept.isEnabled = false
        ConsentStore.get(this).accept(System.currentTimeMillis(), upload.isChecked, telemetry.isChecked)
        setResult(RESULT_OK)
        if (intent.getBooleanExtra(EXTRA_START_SCAN, false)) startActivity(Intent(this, CaptureActivity::class.java))
        finish()
    }

    private fun toggle(label: String) = UiKit.toggle(this, false).apply {
        text = label; textSize = 16f; setTextColor(UiKit.LABEL); isChecked = false
        minHeight = dp(56); setPadding(dp(12), dp(8), dp(12), dp(8))
        background = UiKit.card(context, 18)
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun lp(bottom: Int) = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        .apply { bottomMargin = dp(bottom) }

    companion object {
        const val EXTRA_START_SCAN = "start_scan"
    }
}
