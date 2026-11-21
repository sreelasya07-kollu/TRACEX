package dev.capturekit.consent

import android.content.Context
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/** Recorded consent. [at] is ISO 8601 UTC, written by the shell (the core never reads a clock). */
data class Consent(val version: String, val at: String, val upload: Boolean, val telemetry: Boolean)

/** Minimal key/value seam so the store is JVM-testable. [put] must apply all entries together. */
interface ConsentStorage {
    fun getString(key: String): String?
    fun getBoolean(key: String): Boolean
    fun put(strings: Map<String, String>, booleans: Map<String, Boolean>)
    fun clear()
}

/**
 * Versioned consent. Consent recorded under an older text version counts as none, so a text change re-asks.
 * Upload and telemetry are separate opt-ins (default off) and are readable only through [uploadAllowed] and
 * [telemetryAllowed]; both are false whenever there is no current consent. Revoking clears everything.
 */
class ConsentStore(private val storage: ConsentStorage) {
    /** The current consent, or null when never given, revoked, or recorded under another text version. */
    fun current(): Consent? = synchronized(LOCK) {
        val v = storage.getString(K_VERSION) ?: return@synchronized null
        val at = storage.getString(K_AT) ?: return@synchronized null
        if (v != CURRENT_VERSION || at.isEmpty()) return@synchronized null
        Consent(v, at, storage.getBoolean(K_UPLOAD), storage.getBoolean(K_TELEMETRY))
    }

    fun hasConsent(): Boolean = current() != null
    fun uploadAllowed(): Boolean = current()?.upload == true
    fun telemetryAllowed(): Boolean = current()?.telemetry == true

    /** Records consent to the current text. [nowMs] is epoch ms from the shell clock. */
    fun accept(nowMs: Long, upload: Boolean, telemetry: Boolean) = synchronized(LOCK) {
        storage.put(
            mapOf(K_VERSION to CURRENT_VERSION, K_AT to iso(nowMs)),
            mapOf(K_UPLOAD to upload, K_TELEMETRY to telemetry),
        )
    }

    fun revoke() = synchronized(LOCK) { storage.clear() }

    /**
     * Session config keys for the core: consent_version + consent_at (always together) and redaction_mode.
     * Null without consent, so the caller must not start a scan.
     */
    fun sessionConfig(): Map<String, Any>? {
        val c = current() ?: return null
        return mapOf("consent_version" to c.version, "consent_at" to c.at, "redaction_mode" to REDACTION_MODE)
    }

    companion object {
        /** Matches res/raw/consent_v1.txt. Bump with the text. */
        const val CURRENT_VERSION = "1"
        /** Raw text resource name for [CURRENT_VERSION] (sample-app res/raw). */
        const val TEXT_RESOURCE = "consent_v$CURRENT_VERSION"
        private val LOCK = Any()   // shared by all instances: get() returns a new store per call
        const val REDACTION_MODE = "faces"
        private const val PREFS = "ck_consent"
        private const val K_VERSION = "version"
        private const val K_AT = "at"
        private const val K_UPLOAD = "upload"
        private const val K_TELEMETRY = "telemetry"

        fun iso(epochMs: Long): String =
            SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date(epochMs))

        fun get(context: Context): ConsentStore {
            val p = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            return ConsentStore(object : ConsentStorage {
                override fun getString(key: String): String? = p.getString(key, null)
                override fun getBoolean(key: String): Boolean = p.getBoolean(key, false)
                override fun put(strings: Map<String, String>, booleans: Map<String, Boolean>) {
                    val e = p.edit()
                    strings.forEach { (k, v) -> e.putString(k, v) }
                    booleans.forEach { (k, v) -> e.putBoolean(k, v) }
                    e.commit()
                }
                override fun clear() { p.edit().clear().commit() }
            })
        }
    }
}
