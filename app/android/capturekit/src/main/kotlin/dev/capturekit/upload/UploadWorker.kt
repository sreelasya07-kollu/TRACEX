package dev.capturekit.upload

import android.content.Context
import androidx.work.BackoffPolicy
import androidx.work.Constraints
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.Worker
import androidx.work.WorkerParameters
import androidx.work.workDataOf
import dev.capturekit.consent.ConsentStore
import java.io.File
import java.util.concurrent.TimeUnit

/** Thrown from the progress callback to stop an in-flight upload (consent revoked or work cancelled). */
private class UploadAborted : RuntimeException()

/**
 * Background upload under WorkManager: survives process death and reboot, runs only with a network,
 * backs off exponentially. Consent is checked at enqueue AND on every run (ConsentStore, so a revoke stops work that
 * is already queued) and a FAIL verdict never uploads. Local files are deleted only after the server reports
 * COMPLETE and only if the user's setting allows. State for the UI: [status] (waiting / uploading with percent /
 * retrying / verified / failed), verified also persisted as a marker beside the bundle.
 */
class UploadWorker(ctx: Context, params: WorkerParameters) : Worker(ctx, params) {
    override fun doWork(): Result {
        val dir = File(inputData.getString(KEY_DIR) ?: return Result.failure())
        if (!inputData.getBoolean(KEY_CONSENT, false)) return Result.failure(workDataOf(KEY_ERROR to "no upload consent recorded"))
        val manifest = File(dir, UploadPlan.MANIFEST)
        if (!manifest.exists()) return Result.failure(workDataOf(KEY_ERROR to "bundle not finalized"))
        // Run-time gate: consent may have been revoked since this work was queued.
        when (UploadPolicy.decide(ConsentStore.get(applicationContext).uploadAllowed(), UploadPolicy.verdictOf(manifest.readText()))) {
            UploadPolicy.Decision.NO_CONSENT -> return Result.failure(workDataOf(KEY_ERROR to "upload consent revoked"))
            UploadPolicy.Decision.VERDICT_FAIL -> return Result.failure(workDataOf(KEY_ERROR to "scan check failed: not uploaded"))
            UploadPolicy.Decision.NO_VERDICT -> return Result.failure(workDataOf(KEY_ERROR to "bundle not finalized"))
            UploadPolicy.Decision.RUN -> {}
        }
        var last = -1
        val up = TusUploader(applicationContext, inputData.getString(KEY_TUS) ?: return Result.failure(), inputData.getString(KEY_VERIFIER),
            onProgress = { done, total ->
                // Revoke (or cancel) while in flight: stop at the next chunk. tus keeps the offset, nothing more is sent.
                if (isStopped || !ConsentStore.get(applicationContext).uploadAllowed()) throw UploadAborted()
                val p = UploadPolicy.percent(done, total)
                if (p != last) { last = p; setProgressAsync(workDataOf(KEY_PROGRESS to p)) }
            })
        val outcome = try { up.uploadBundle(dir) } catch (e: UploadAborted) {
            return if (isStopped) Result.failure() else Result.failure(workDataOf(KEY_ERROR to "upload consent revoked"))
        }
        return when (val o = outcome) {
            is TusUploader.Outcome.Complete -> {
                val marker = UploadPolicy.verifiedMarker(dir)
                if (inputData.getBoolean(KEY_DELETE, false)) {
                    dir.deleteRecursively()
                    dev.capturekit.capture.ResearchReport.deleteSiblings(dir)   // field research file lives beside the bundle
                    marker.delete()
                } else {
                    try { marker.writeText("verified") } catch (_: java.io.IOException) { }   // survives WorkManager pruning finished work
                }
                Result.success()
            }
            is TusUploader.Outcome.Retry -> Result.retry()
            is TusUploader.Outcome.Failed -> Result.failure(workDataOf(KEY_ERROR to o.reason))
        }
    }

    companion object {
        const val KEY_DIR = "dir"; const val KEY_TUS = "tus"; const val KEY_VERIFIER = "verifier"
        const val KEY_CONSENT = "consent"; const val KEY_DELETE = "delete_after_verify"; const val KEY_ERROR = "error"
        const val KEY_PROGRESS = "progress"
        private const val TAG = "capturekit-upload"

        private fun name(bundle: File) = "upload-${bundle.name}"

        /**
         * One unique job per bundle. [replace] = false: enqueueing while pending is a no-op (KEEP; a finished job is
         * replaced, so Retry after a failure works). [replace] = true restarts it (new network constraint); tus resumes.
         */
        fun enqueue(ctx: Context, bundle: File, tusUrl: String, verifierUrl: String?, consent: Boolean, deleteAfterVerify: Boolean = false,
                    unmeteredOnly: Boolean = false, replace: Boolean = false) {
            val req = OneTimeWorkRequestBuilder<UploadWorker>()
                .setInputData(workDataOf(KEY_DIR to bundle.absolutePath, KEY_TUS to tusUrl, KEY_VERIFIER to verifierUrl,
                    KEY_CONSENT to consent, KEY_DELETE to deleteAfterVerify))
                .setConstraints(Constraints.Builder().setRequiredNetworkType(if (unmeteredOnly) NetworkType.UNMETERED else NetworkType.CONNECTED).build())
                .addTag(TAG)
                .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 10, TimeUnit.SECONDS)
                .build()
            WorkManager.getInstance(ctx).enqueueUniqueWork(name(bundle), if (replace) ExistingWorkPolicy.REPLACE else ExistingWorkPolicy.KEEP, req)
        }

        /** Stops every queued or running upload (consent withdrawn). Safe on any thread. */
        fun cancelAll(ctx: Context) { WorkManager.getInstance(ctx).cancelAllWorkByTag(TAG) }

        /** Stops one queued/running upload. */
        fun cancel(ctx: Context, bundle: File) { WorkManager.getInstance(ctx).cancelUniqueWork(name(bundle)) }

        /** Blocking read of WorkManager: call off the main thread. NONE when nothing was ever queued for this bundle. */
        fun status(ctx: Context, bundle: File): UploadStatus {
            if (UploadPolicy.verifiedMarker(bundle).exists()) return UploadStatus(UploadState.VERIFIED, 100)
            val info = try { WorkManager.getInstance(ctx).getWorkInfosForUniqueWork(name(bundle)).get().lastOrNull() } catch (t: Throwable) { null }
                ?: return UploadStatus(UploadState.NONE)
            val st = UploadState.fromWork(info.state, info.runAttemptCount)
            return UploadStatus(st, if (st == UploadState.UPLOADING) info.progress.getInt(KEY_PROGRESS, 0) else if (st == UploadState.VERIFIED) 100 else -1,
                if (st == UploadState.FAILED) info.outputData.getString(KEY_ERROR) else null)
        }
    }
}
