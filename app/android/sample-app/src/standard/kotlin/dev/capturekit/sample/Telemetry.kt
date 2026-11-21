package dev.capturekit.sample

import android.content.Context

/** Standard flavour: no crash reporting, no network, no Sentry classes. Same API as the field implementation. */
@Suppress("UNUSED_PARAMETER")
object Telemetry {
    fun scrub(s: String): String = s
    fun sync(context: Context) {}
}
