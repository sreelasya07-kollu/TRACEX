package dev.capturekit.sample.upload

import android.content.Context
import dev.capturekit.consent.ConsentStore
import dev.capturekit.sample.BuildConfig
import dev.capturekit.upload.UploadPolicy
import dev.capturekit.upload.UploadSettings
import dev.capturekit.upload.UploadWorker
import java.io.File

/**
 * Field flavour only (needs BuildConfig.TUS_URL and the INTERNET permission). Queues the tus upload when the user
 * consented to upload and the verdict is not FAIL; the worker re-checks consent on every run. Same API as standard.
 */
object UploadGate {
    val available: Boolean get() = BuildConfig.TUS_URL.isNotBlank()

    /** True when queued. Reads the consent store: call off the main thread is fine but not required. */
    fun enqueueIfAllowed(ctx: Context, bundle: File, verdict: String?, replace: Boolean = false): Boolean {
        if (!available) return false
        if (UploadPolicy.verifiedMarker(bundle).exists()) return false   // already uploaded and verified
        if (!UploadPolicy.eligible(ConsentStore.get(ctx).uploadAllowed(), verdict)) return false
        UploadWorker.enqueue(ctx, bundle, BuildConfig.TUS_URL, BuildConfig.VERIFIER_URL.ifBlank { null }, consent = true,
            unmeteredOnly = UploadSettings.unmeteredOnly(ctx), replace = replace)
        return true
    }
}
