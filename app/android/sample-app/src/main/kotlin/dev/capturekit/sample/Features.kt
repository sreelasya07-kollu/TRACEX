package dev.capturekit.sample

import android.content.Context
import android.content.pm.ApplicationInfo
import android.util.Log
import dev.capturekit.capture.FeatureFlags

/**
 * The app's feature switches (ADR 0032), shared by every screen. Release builds: [FeatureFlags.Flags.DEFAULTS], no
 * property read. Debuggable builds: all `debug.capturekit.*` properties from ONE argument-less `getprop` call, parsed by
 * [FeatureFlags.parseGetprop], lazily on first use and cached for the process (force-stop the app after `adb shell setprop`).
 */
object Features {
    private const val TAG = "CKFeatures"
    @Volatile private var cached: FeatureFlags.Flags? = null

    fun get(ctx: Context): FeatureFlags.Flags = cached ?: synchronized(this) {
        cached ?: run {
            val debuggable = ctx.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0
            val props = if (debuggable) FeatureFlags.parseGetprop(getpropAll()) else emptyMap()
            (if (debuggable) FeatureFlags.fromProps { props[it] } else FeatureFlags.Flags.DEFAULTS).also {
                cached = it
                Log.i(TAG, "features $it")
            }
        }
    }

    /** Warm the cache on a background thread (called from the launcher) so the capture screen's onCreate rarely waits on getprop. */
    fun prefetch(ctx: Context) {
        if (cached != null) return
        val app = ctx.applicationContext
        Thread({ get(app) }, "ck-features").start()
    }

    /** One `getprop` child process listing every property; streams closed, process destroyed; "" on any failure. */
    private fun getpropAll(): String {
        var p: Process? = null
        return try {
            p = Runtime.getRuntime().exec(arrayOf("getprop"))
            p.inputStream.bufferedReader().use { it.readText() }
        } catch (t: Throwable) { "" } finally { p?.destroy() }
    }
}
