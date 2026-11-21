package dev.capturekit.upload

import android.content.Context

/** User's upload network preference. Default: unmetered (Wi-Fi) only, because a scan bundle is large. */
object UploadSettings {
    private const val PREFS = "ck_upload"
    private const val K_UNMETERED = "unmetered_only"

    fun unmeteredOnly(ctx: Context): Boolean =
        ctx.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(K_UNMETERED, true)

    fun setUnmeteredOnly(ctx: Context, v: Boolean) {
        ctx.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(K_UNMETERED, v).apply()
    }
}
