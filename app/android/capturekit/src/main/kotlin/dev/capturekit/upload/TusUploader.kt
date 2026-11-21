package dev.capturekit.upload

import android.content.Context
import io.tus.android.client.TusPreferencesURLStore
import io.tus.java.client.TusClient
import io.tus.java.client.TusExecutor
import io.tus.java.client.TusUpload
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * tus upload of a Scan Bundle: one resumable upload per file (metadata bundle_id, path, sha256),
 * manifest.json LAST, then wait for the server's verdict (verifier GET /bundles/<id>).
 * Upload URLs live in SharedPreferences (TusPreferencesURLStore), so a killed process or a reboot
 * resumes from the server's offset. Transient failures retry with exponential backoff.
 * Mirrors tools/ckit/upload.py (the reference implementation exercised by the docker failure matrix).
 */
class TusUploader(
    context: Context,
    private val tusUrl: String,
    private val verifierUrl: String?,
    private val chunkSize: Int = 256 * 1024,
    /** (bytes sent so far, total bytes) over the whole bundle; called on the worker thread after every chunk. */
    private val onProgress: ((Long, Long) -> Unit)? = null,
) {
    private var doneBefore = 0L
    private var total = 0L

    private val prefs = context.applicationContext.getSharedPreferences("capturekit_tus", Context.MODE_PRIVATE)
    private val store = TusPreferencesURLStore(prefs)
    private val client = TusClient().apply {
        uploadCreationURL = URL(tusUrl)
        enableResuming(store)
    }

    sealed class Outcome {
        object Complete : Outcome()
        data class Retry(val reason: String) : Outcome()   // network/server trouble: WorkManager backs off
        data class Failed(val reason: String) : Outcome()  // CORRUPT after repair: user-visible failure
    }

    private fun uploadFile(bundle: File, rel: String, bundleId: String, fresh: Boolean) {
        val f = File(bundle, rel)
        val up = TusUpload(f).apply {
            metadata = mapOf("bundle_id" to bundleId, "path" to rel, "sha256" to UploadPlan.sha256(f), "filename" to f.name)
        }
        if (fresh) store.remove(up.fingerprint)  // damaged on the server: start over, do not resume
        object : TusExecutor() {
            override fun makeAttempt() {
                val uploader = client.resumeOrCreateUpload(up)
                uploader.chunkSize = chunkSize
                do { onProgress?.invoke((doneBefore + uploader.offset).coerceAtMost(total), total) } while (uploader.uploadChunk() > -1)
                uploader.finish()
            }
        }.apply { setDelays(intArrayOf(500, 1000, 2000, 4000, 8000)) }.makeAttempts()
    }

    private fun status(bundleId: String): ServerStatus {
        val c = URL("$verifierUrl/bundles/$bundleId").openConnection() as HttpURLConnection
        c.connectTimeout = 10_000; c.readTimeout = 20_000
        return c.inputStream.use { ServerStatus.parse(it.readBytes().decodeToString()) }
    }

    /** Runs the whole upload. IOExceptions (offline, server restart) surface as Retry. */
    fun uploadBundle(bundle: File, waitMs: Long = 30_000): Outcome = try {
        val id = UploadPlan.bundleId(File(bundle, UploadPlan.MANIFEST).readText()) ?: return Outcome.Failed("manifest has no bundle_id")
        val files = UploadPlan.order(bundle)
        total = files.sumOf { File(bundle, it).length() }; doneBefore = 0L
        files.forEach { uploadFile(bundle, it, id, fresh = false); doneBefore += File(bundle, it).length() }
        onProgress?.invoke(total, total)
        if (verifierUrl == null) Outcome.Complete else waitForVerdict(bundle, id, waitMs)
    } catch (e: java.io.IOException) {
        Outcome.Retry(e.message ?: e.javaClass.simpleName)
    } catch (e: io.tus.java.client.ProtocolException) {
        Outcome.Retry(e.message ?: "tus protocol error")
    }

    private fun waitForVerdict(bundle: File, id: String, waitMs: Long): Outcome {
        val end = System.currentTimeMillis() + waitMs
        var repaired = false
        while (true) {
            val s = status(id)
            when {
                s.status == "COMPLETE" -> return Outcome.Complete
                s.status == "CORRUPT" && !repaired -> {
                    repaired = true  // re-send damaged files once, manifest again last
                    (s.corruptPaths.filter { it != UploadPlan.MANIFEST } + UploadPlan.MANIFEST).forEach { uploadFile(bundle, it, id, fresh = true) }
                }
                s.status == "CORRUPT" -> return Outcome.Failed(s.reason)
                s.status == "INCOMPLETE" -> s.missing.forEach { uploadFile(bundle, it, id, fresh = true) }
            }
            if (System.currentTimeMillis() > end) return Outcome.Retry("server status ${s.status}: ${s.reason}")
            Thread.sleep(500)
        }
    }
}
