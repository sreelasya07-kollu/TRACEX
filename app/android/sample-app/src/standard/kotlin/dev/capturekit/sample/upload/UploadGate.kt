package dev.capturekit.sample.upload

import android.content.Context
import java.io.File

/** Standard flavour: offline build (INTERNET is stripped from the manifest), so nothing is ever queued. Same API as field. */
@Suppress("UNUSED_PARAMETER")
object UploadGate {
    val available: Boolean get() = false
    fun enqueueIfAllowed(ctx: Context, bundle: File, verdict: String?, replace: Boolean = false): Boolean = false
}
