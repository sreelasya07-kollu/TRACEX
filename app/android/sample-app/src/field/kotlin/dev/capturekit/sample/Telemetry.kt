package dev.capturekit.sample

import android.content.Context
import dev.capturekit.consent.ConsentStore
import dev.capturekit.privacy.Scrub
import io.sentry.Sentry
import io.sentry.SentryEvent
import io.sentry.SentryOptions
import io.sentry.android.core.SentryAndroid

/**
 * Field flavour crash/error reports. Off unless BOTH a DSN is built in (-PSENTRY_DSN=..., empty = off) and the
 * current consent allows telemetry. [sync] is safe to call repeatedly (launch, after consent changes): it starts
 * Sentry when allowed and closes it when consent is withdrawn. Events are scrubbed of file paths and bundle names.
 */
object Telemetry {
    private var started = false

    /** Replaces file system paths and bundle names in [s]. */
    fun scrub(s: String): String = Scrub.scrub(s)

    @Synchronized
    fun sync(context: Context) {
        val allowed = BuildConfig.SENTRY_DSN.isNotEmpty() && ConsentStore.get(context).telemetryAllowed()
        if (allowed && !started) {
            val store = ConsentStore.get(context)
            SentryAndroid.init(context.applicationContext) { o ->
                o.dsn = BuildConfig.SENTRY_DSN
                o.isSendDefaultPii = false
                // Re-checked per event: after a revoke nothing is sent even before sync() closes the client.
                o.beforeSend = SentryOptions.BeforeSendCallback { e, _ -> if (store.telemetryAllowed()) scrubEvent(e) else null }
                o.beforeBreadcrumb = SentryOptions.BeforeBreadcrumbCallback { b, _ ->
                    b.message = b.message?.let(::scrub)
                    b.data.entries.forEach { (k, v) -> if (v is String) b.data[k] = scrub(v) }
                    b
                }
            }
            started = true
        } else if (!allowed && started) {
            Sentry.close()
            started = false
        }
    }

    private fun scrubEvent(e: SentryEvent): SentryEvent {
        e.message?.let { m -> m.message = m.message?.let(::scrub); m.formatted = m.formatted?.let(::scrub) }
        e.exceptions?.forEach { x ->
            x.value = x.value?.let(::scrub)
            x.stacktrace?.frames?.forEach { f -> f.absPath = null; f.filename = f.filename?.let(::scrub) }
        }
        e.threads?.forEach { t -> t.stacktrace?.frames?.forEach { f -> f.absPath = null } }
        e.extras?.let { ex -> ex.entries.toList().forEach { (k, v) -> if (v is String) ex[k] = scrub(v) } }
        e.tags?.let { tg -> tg.entries.toList().forEach { (k, v) -> tg[k] = scrub(v) } }
        e.serverName = null
        return e
    }
}
