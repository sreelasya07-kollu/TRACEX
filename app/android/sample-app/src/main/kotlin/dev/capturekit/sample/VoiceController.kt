package dev.capturekit.sample

import android.Manifest
import android.app.Activity
import android.app.Dialog
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.speech.RecognitionListener
import android.speech.RecognitionSupport
import android.speech.RecognitionSupportCallback
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import android.util.Log
import android.view.Gravity
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import dev.capturekit.guidance.Confirmation
import dev.capturekit.guidance.Hypothesis
import dev.capturekit.guidance.PhraseTables
import dev.capturekit.guidance.ScanStateSnapshot
import dev.capturekit.guidance.StateAnswers
import dev.capturekit.guidance.VoiceIntent
import dev.capturekit.guidance.VoiceIntentParser
import dev.capturekit.guidance.VoiceParse
import dev.capturekit.sample.UiKit.dp

/** What the mic chip shows. */
enum class MicState { IDLE, LISTENING, THINKING, NEEDS_PERMISSION }

/**
 * Push-to-talk voice commands (Phase 10) on the platform on-device recogniser: API 31+, English pack installed,
 * no network path (never calls triggerModelDownload), RECORD_AUDIO asked only after a plain-language rationale.
 * The recogniser is created, used and destroyed on the main thread only: every public method hops to it.
 * Guidance speech is held off while listening so the app never hears itself. "Finish" always needs a confirmation
 * (tap, or a spoken yes/no), because it ends the scan. Phrases and parsing live in the tested lib
 * ([VoiceIntentParser]); "what is missing" is answered by [StateAnswers] from the last overlay snapshot.
 * Latency is logged under tag CK/Voice as "speech end -> action N ms".
 */
class VoiceController(
    private val activity: Activity,
    private val speech: SpeechRenderer,
    private val host: Host,
) {
    /** The scan screen's side of the contract. All calls happen on the main thread. */
    interface Host {
        fun isPaused(): Boolean
        fun pauseScan()
        fun resumeScan()
        /** Called only after the user confirmed (tap or spoken yes). */
        fun finishConfirmed()
        fun snapshot(): ScanStateSnapshot?
        /** Short message under the top bar; null clears it. */
        fun feedback(text: String?)
        /** [micState] or [available] changed: refresh the chip. */
        fun voiceStateChanged()
    }

    /** Called (main thread) for every recognised command, before it runs; the field research log counts them. */
    var onIntent: (VoiceIntent) -> Unit = {}

    private enum class Mode { COMMAND, CONFIRM }

    private val app: Context = activity.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val parser = VoiceIntentParser(requireNotNull(PhraseTables.forLocale("en")))
    private var recognizer: SpeechRecognizer? = null
    private var state = MicState.IDLE
    private var mode = Mode.COMMAND
    private var lang = LANG_PREFERRED
    private var fellBackLang = false
    private var permissionDenied = false
    private var permissionAsked = false   // rationale answered or system dialog shown this session: never nag again
    private var speechEndMs = 0L
    private var gotResult = false
    private var released = false
    private var confirmDialog: Dialog? = null
    private var rationaleDialog: Dialog? = null
    private val parkedFeedbackClear = Runnable { host.feedback(null) }
    private val listenTimeout = Runnable { onListenTimeout() }
    private val hardTimeout = Runnable { onHardTimeout() }

    /** True once an on-device recogniser with an English pack exists. The mic chip is hidden until then. */
    var available = false
        private set

    /** Plain reason when [available] is false (null while the probe is still running). */
    var unavailableReason: String? = null
        private set

    val micState: MicState
        get() = if (state == MicState.IDLE && !granted()) MicState.NEEDS_PERMISSION else state

    init { main.post { probe() } }   // posted: runs after the constructor, so every property below is initialised

    // ---- availability ------------------------------------------------------------------------------------------

    private fun probe() {
        if (released) return
        if (Build.VERSION.SDK_INT < 31) { unavailable("Voice commands need Android 12 or newer. The buttons still work."); return }
        if (!SpeechRecognizer.isOnDeviceRecognitionAvailable(app)) {
            unavailable("This phone has no offline speech recogniser. The buttons still work."); return
        }
        val r = try { SpeechRecognizer.createOnDeviceSpeechRecognizer(app) } catch (e: RuntimeException) {
            Log.w(TAG, "createOnDeviceSpeechRecognizer failed", e)
            unavailable("Voice commands could not start on this phone. The buttons still work."); return
        }
        r.setRecognitionListener(listener)
        recognizer = r
        if (Build.VERSION.SDK_INT >= 33) {
            // Language-pack check never downloads anything; no English pack = no mic (Settled: no silent download).
            r.checkRecognitionSupport(recognizerIntent("en-US"), app.mainExecutor, object : RecognitionSupportCallback {
                override fun onSupportResult(support: RecognitionSupport) {
                    val installed = support.installedOnDeviceLanguages.map { it.replace('_', '-') }
                    val pick = LANG_PREFERRED_ORDER.firstOrNull { p -> installed.any { it.equals(p, ignoreCase = true) } }
                        ?: installed.firstOrNull { it.startsWith("en", ignoreCase = true) }
                    if (pick == null) unavailable("Offline English speech is not installed on this phone. The buttons still work.")
                    else { lang = pick; fellBackLang = true; markAvailable() }
                }
                override fun onError(error: Int) {
                    Log.w(TAG, "checkRecognitionSupport error $error")
                    unavailable("Voice commands are not available offline on this phone. The buttons still work.")
                }
            })
        } else {
            // KNOWN GAP (API 31-32): there is no silent way to ask whether an offline pack is installed (checkRecognitionSupport
            // is API 33), and probing needs a recognition session. So the mic chip shows here, and the first language error
            // (after one en-US retry) hides it for this session and shows NEEDS_PACK. API 33+ never shows it without a pack.
            markAvailable()
        }
    }

    private fun markAvailable() { available = true; unavailableReason = null; Log.i(TAG, "voice commands available, lang=$lang"); host.voiceStateChanged() }

    private fun unavailable(why: String) {
        available = false; unavailableReason = why
        Log.i(TAG, "voice commands unavailable: $why")
        destroyRecognizer()
        host.voiceStateChanged()
    }

    // ---- push to talk ------------------------------------------------------------------------------------------

    /** Mic chip tapped. */
    fun onMicTapped() = onMain {
        if (released || !available) return@onMain
        when (state) {
            MicState.LISTENING -> { cancelListening(); host.feedback(null) }
            MicState.THINKING -> Unit
            else -> beginCommand()
        }
    }

    /** Set while the rationale/system dialog asked from [ensurePermissionThen] is open; run once, whatever the answer. */
    private var proceedAfterPermission: (() -> Unit)? = null

    /**
     * Called by the scan screen when Start is pressed: asks for RECORD_AUDIO (rationale first) BEFORE the scan runs, so
     * the system dialog can never interrupt a running scan. [proceed] always runs exactly once: at once when voice is
     * unavailable, the permission is already held, or it was already asked this session; otherwise after the user
     * answers. A refusal only turns voice commands off. A second call while a dialog is open is ignored.
     */
    fun ensurePermissionThen(proceed: () -> Unit) = onMain {
        if (released) return@onMain
        if (proceedAfterPermission != null) return@onMain   // dialog already open
        if (!available || granted() || permissionAsked) { proceed(); return@onMain }
        proceedAfterPermission = proceed
        showRationale()
    }

    private fun runPendingProceed() { val p = proceedAfterPermission; proceedAfterPermission = null; p?.invoke() }

    /** From Activity.onRequestPermissionsResult for [REQ_CODE]. */
    fun onPermissionResult(grantResults: IntArray) = onMain {
        if (grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            permissionDenied = false
            host.voiceStateChanged()
        } else {
            permissionDenied = true
            val blocked = !activity.shouldShowRequestPermissionRationale(Manifest.permission.RECORD_AUDIO)
            showFeedback("The microphone is off, so voice commands are off. " +
                (if (blocked) "Allow it in the phone's app settings. " else "") + "The buttons still work.", 8_000)
            host.voiceStateChanged()
        }
        runPendingProceed()
    }

    /** Leaving the screen: stop listening and drop any open question. */
    fun onPause() = onMain { cancelListening(); dismissDialogs() }

    fun release() = onMain {
        released = true
        main.removeCallbacksAndMessages(null)
        cancelListening()
        dismissDialogs()
        destroyRecognizer()
    }

    private fun granted() = activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    private fun beginCommand() {
        if (!granted()) {
            // Never a permission dialog mid-scan (it would pause the scan): it is asked once, before Start.
            showFeedback("Voice commands need the microphone. Allow it in the phone's app settings. The buttons still work.", 8_000)
            return
        }
        startListening(Mode.COMMAND)
    }

    private fun showRationale() {
        if (activity.isFinishing || activity.isDestroyed || rationaleDialog != null) return
        rationaleDialog = simpleDialog(
            title = "Use your voice?",
            body = "Voice commands let you say pause, resume, repeat, what is missing, finish, louder or quieter " +
                "without touching the screen. The microphone listens only after you tap Mic, and speech is " +
                "recognised on this phone with no network. No audio is saved.",
            positive = "Allow microphone", negative = "Not now",
            onPositive = {
                rationaleDialog = null; permissionAsked = true
                activity.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_CODE)
            },
            onNegative = { rationaleDialog = null; permissionAsked = true; permissionDenied = true; host.voiceStateChanged(); runPendingProceed() },
        )
    }

    private fun startListening(m: Mode) {
        val r = recognizer ?: return
        if (state != MicState.IDLE) return
        mode = m
        gotResult = false
        speechEndMs = 0L
        speech.holdOff = true   // before stopNow: nothing may start speaking in between
        speech.stopNow()
        setState(MicState.LISTENING)
        if (m == Mode.COMMAND) host.feedback(null)
        main.removeCallbacks(listenTimeout); main.postDelayed(listenTimeout, LISTEN_MS)
        try {
            r.startListening(recognizerIntent(lang))
        } catch (e: RuntimeException) {
            Log.w(TAG, "startListening failed", e)
            finishListening()
            showFeedback("Could not start listening. Try again.", 4_000)
        }
    }

    private fun onListenTimeout() {
        if (state != MicState.LISTENING) return
        // Ask for whatever was heard so far; if the engine stays silent, hard timeout cancels.
        speechEndMs = SystemClock.elapsedRealtime()
        setState(MicState.THINKING)
        try { recognizer?.stopListening() } catch (e: RuntimeException) { Log.w(TAG, "stopListening", e) }
        main.removeCallbacks(hardTimeout); main.postDelayed(hardTimeout, HARD_MS)
    }

    private fun onHardTimeout() {
        if (state == MicState.IDLE) return
        Log.i(TAG, "no recogniser result after timeout: cancelled")
        cancelListening()
        showFeedback(NOT_HEARD, 5_000)
    }

    private fun cancelListening() {
        if (state != MicState.IDLE) try { recognizer?.cancel() } catch (e: RuntimeException) { Log.w(TAG, "cancel", e) }
        finishListening()
    }

    private fun finishListening() {
        main.removeCallbacks(listenTimeout); main.removeCallbacks(hardTimeout)
        speech.holdOff = false
        setState(MicState.IDLE)
    }

    private fun setState(s: MicState) { if (state != s) { state = s; host.voiceStateChanged() } }

    private fun destroyRecognizer() {
        val r = recognizer ?: return
        recognizer = null
        try { r.destroy() } catch (e: RuntimeException) { Log.w(TAG, "destroy", e) }
    }

    private fun recognizerIntent(tag: String) = android.content.Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
        putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
        putExtra(RecognizerIntent.EXTRA_LANGUAGE, tag)
        putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 5)
        putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, false)
        putExtra(RecognizerIntent.EXTRA_PREFER_OFFLINE, true)
        putExtra(RecognizerIntent.EXTRA_CALLING_PACKAGE, app.packageName)
    }

    // ---- recogniser callbacks (main thread: the recogniser was created there) ----------------------------------

    private val listener = object : RecognitionListener {
        override fun onReadyForSpeech(params: Bundle?) {}
        override fun onBeginningOfSpeech() {}
        override fun onRmsChanged(rmsdB: Float) {}
        override fun onBufferReceived(buffer: ByteArray?) {}
        override fun onPartialResults(partialResults: Bundle?) {}
        override fun onEvent(eventType: Int, params: Bundle?) {}
        override fun onEndOfSpeech() {
            if (state != MicState.LISTENING) return
            speechEndMs = SystemClock.elapsedRealtime()
            setState(MicState.THINKING)
            main.removeCallbacks(listenTimeout)
            main.removeCallbacks(hardTimeout); main.postDelayed(hardTimeout, HARD_MS)
        }
        override fun onResults(results: Bundle?) { onMain { handleResults(results) } }
        override fun onError(error: Int) { onMain { handleError(error) } }
    }

    private fun handleResults(results: Bundle?) {
        if (state == MicState.IDLE) return   // cancelled meanwhile
        gotResult = true
        val texts = results?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION).orEmpty()
        val conf = results?.getFloatArray(SpeechRecognizer.CONFIDENCE_SCORES)
        val nBest = texts.mapIndexed { i, t -> Hypothesis(t, conf?.getOrNull(i)) }
        val m = mode
        val t0 = if (speechEndMs != 0L) speechEndMs else SystemClock.elapsedRealtime()
        finishListening()
        if (m == Mode.CONFIRM) { handleConfirmation(parser.parseConfirmation(nBest), t0); return }
        when (val p = parser.parse(nBest)) {
            is VoiceParse.Match -> {
                act(p.intent)
                Log.i(TAG, "speech end -> action ${SystemClock.elapsedRealtime() - t0} ms intent=${p.intent} score=${"%.2f".format(p.score)}")
            }
            is VoiceParse.NoMatch -> {
                Log.i(TAG, "no command (${p.reason}), ${SystemClock.elapsedRealtime() - t0} ms")
                showFeedback(NOT_UNDERSTOOD, 6_000)
            }
        }
    }

    private fun handleError(code: Int) {
        if (state == MicState.IDLE) return   // we cancelled: the engine reports ERROR_CLIENT afterwards
        val m = mode
        Log.i(TAG, "recogniser error $code")
        if ((code == ERROR_LANGUAGE_NOT_SUPPORTED || code == ERROR_LANGUAGE_UNAVAILABLE) && !fellBackLang) {
            fellBackLang = true; lang = LANG_FALLBACK
            finishListening(); startListening(m)   // one retry with en-US
            return
        }
        finishListening()
        when (code) {
            SpeechRecognizer.ERROR_NO_MATCH, SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> showFeedback(NOT_HEARD, 5_000)
            SpeechRecognizer.ERROR_AUDIO -> showFeedback("Microphone problem. Tap Mic to try again.", 5_000)
            SpeechRecognizer.ERROR_RECOGNIZER_BUSY, ERROR_TOO_MANY_REQUESTS -> showFeedback("Voice is busy. Tap Mic again in a moment.", 5_000)
            SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> {
                permissionDenied = true
                showFeedback("The microphone is off, so voice commands are off. The buttons still work.", 8_000)
                host.voiceStateChanged()
            }
            ERROR_LANGUAGE_NOT_SUPPORTED, ERROR_LANGUAGE_UNAVAILABLE -> {
                unavailable("Offline English speech is not installed on this phone. The buttons still work.")
                showFeedback(NEEDS_PACK, 8_000)
            }
            SpeechRecognizer.ERROR_NETWORK, SpeechRecognizer.ERROR_NETWORK_TIMEOUT, SpeechRecognizer.ERROR_SERVER, ERROR_SERVER_DISCONNECTED ->
                unavailable("Voice commands are not available offline on this phone. The buttons still work.")
            else -> showFeedback("Voice command failed. Tap Mic to try again.", 5_000)
        }
    }

    // ---- actions -----------------------------------------------------------------------------------------------

    private fun act(intent: VoiceIntent) {
        onIntent(intent)
        when (intent) {
            VoiceIntent.PAUSE ->
                if (host.isPaused()) showFeedback("Already paused.", 3_000) else { host.pauseScan(); showFeedback("Paused.", 3_000) }
            VoiceIntent.RESUME ->
                if (!host.isPaused()) showFeedback("Already scanning.", 3_000) else { host.resumeScan(); showFeedback("Resuming.", 3_000) }
            VoiceIntent.REPEAT -> {
                val last = speech.repeatLast()
                if (!last) showFeedback(if (!speech.enabled) "Voice guidance is off. Turn it on to hear it again." else "Nothing to repeat yet.", 4_000)
            }
            VoiceIntent.MISSING -> {
                val a = StateAnswers.answer(StateAnswers.questionFor(intent), host.snapshot())
                showFeedback(a.text, 9_000)   // always visible, also when voice guidance is muted
                speech.say(a.text)
            }
            VoiceIntent.FINISH -> confirmFinish()
            VoiceIntent.LOUDER -> volume(speech.louder(), "Louder.", "Already at the loudest.")
            VoiceIntent.QUIETER -> volume(speech.quieter(), "Quieter.", "Already at the quietest.")
        }
    }

    private fun volume(changed: Boolean, done: String, limit: String) {
        val msg = if (changed) done else limit
        showFeedback(msg, 3_000)
        speech.say(msg, remember = false)   // spoken at the new level so the change is audible
    }

    private fun confirmFinish() {
        if (activity.isFinishing || activity.isDestroyed || confirmDialog != null) return
        confirmDialog = simpleDialog(
            title = "Finish the scan?",
            body = "Say yes to finish, or no to keep scanning. You can also tap a button.",
            positive = "Finish", negative = "Keep scanning",
            onPositive = { endConfirm(); host.finishConfirmed() },
            onNegative = { endConfirm() },
        )
        // Spoken yes/no right away, no TTS in between (the question is on screen): the user just spoke.
        if (state == MicState.IDLE) startListening(Mode.CONFIRM)
    }

    private fun handleConfirmation(c: Confirmation, t0: Long) {
        Log.i(TAG, "speech end -> confirmation $c ${SystemClock.elapsedRealtime() - t0} ms")
        when (c) {
            Confirmation.YES -> { endConfirm(); host.finishConfirmed() }
            Confirmation.NO -> { endConfirm(); showFeedback("Okay, still scanning.", 3_000) }
            Confirmation.UNKNOWN -> showFeedback("Tap Finish or Keep scanning.", 5_000)
        }
    }

    private fun endConfirm() {
        closeQuietly(confirmDialog); confirmDialog = null
        if (mode == Mode.CONFIRM && state != MicState.IDLE) cancelListening()
    }

    private fun dismissDialogs() {
        closeQuietly(confirmDialog); confirmDialog = null
        if (rationaleDialog != null) proceedAfterPermission = null   // screen left before an answer: the user presses Start again
        closeQuietly(rationaleDialog); rationaleDialog = null
    }

    /** Dismiss without running the dialog's own dismiss handler (it would re-enter the callers above). */
    private fun closeQuietly(d: Dialog?) { d?.setOnDismissListener(null); d?.dismiss() }

    private fun showFeedback(text: String, ms: Long) {
        host.feedback(text)
        main.removeCallbacks(parkedFeedbackClear); main.postDelayed(parkedFeedbackClear, ms)
    }

    /** Two-button dialog in the review sheet's dark style. Dismissing by back counts as the negative button. */
    private fun simpleDialog(title: String, body: String, positive: String, negative: String, onPositive: () -> Unit, onNegative: () -> Unit): Dialog {
        val dlg = Dialog(activity)
        fun text(t: String, sp: Float, bold: Boolean = false) = TextView(activity).apply {
            this.text = t; textSize = sp; setTextColor(Color.WHITE)
            if (bold) typeface = Typeface.DEFAULT_BOLD
        }
        val wrap = ViewGroup.LayoutParams.WRAP_CONTENT
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            background = UiKit.rounded(0xFF101418.toInt(), activity.dp(UiKit.RADIUS_DP).toFloat())
            setPadding(activity.dp(20), activity.dp(16), activity.dp(20), activity.dp(16))
            addView(text(title, UiKit.TEXT_L, true))
            addView(text(body, UiKit.TEXT_M), LinearLayout.LayoutParams(wrap, wrap).apply { topMargin = activity.dp(8) })
            val btns = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.END }
            btns.addView(UiKit.bigButton(activity, negative, 0xFF455A64.toInt()).apply { setOnClickListener { dlg.dismiss() } })
            btns.addView(UiKit.bigButton(activity, positive).apply {
                setOnClickListener { dlg.setOnDismissListener(null); dlg.dismiss(); onPositive() }
            }, LinearLayout.LayoutParams(wrap, wrap).apply { marginStart = activity.dp(12) })
            addView(btns, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, wrap).apply { topMargin = activity.dp(16) })
        }
        dlg.window?.setBackgroundDrawableResource(android.R.color.transparent)
        dlg.setContentView(root)
        dlg.setOnDismissListener { onNegative() }
        dlg.show()
        return dlg
    }

    private fun onMain(block: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) block() else main.post(block)
    }

    companion object {
        const val REQ_CODE = 2   // CameraPermission uses 1
        private const val TAG = SpeechRenderer.TAG   // "CK/Voice"
        private const val LISTEN_MS = 6_000L
        private const val HARD_MS = 2_500L
        private const val LANG_PREFERRED = "en-IN"
        private const val LANG_FALLBACK = "en-US"
        private val LANG_PREFERRED_ORDER = listOf(LANG_PREFERRED, LANG_FALLBACK)
        private const val NEEDS_PACK = "Voice needs an offline English pack"
        private const val NOT_HEARD = "I did not hear a command. Tap Mic and try again."
        private const val NOT_UNDERSTOOD = "I did not catch that. Say pause, resume, repeat, what is missing, finish, louder or quieter."

        // SpeechRecognizer.ERROR_* added after API 26: literals keep lint quiet and match the SDK values.
        private const val ERROR_TOO_MANY_REQUESTS = 10
        private const val ERROR_SERVER_DISCONNECTED = 11
        private const val ERROR_LANGUAGE_NOT_SUPPORTED = 12
        private const val ERROR_LANGUAGE_UNAVAILABLE = 13
    }
}
