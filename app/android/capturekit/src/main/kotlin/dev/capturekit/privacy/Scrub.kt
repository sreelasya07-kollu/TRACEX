package dev.capturekit.privacy

/**
 * Pure text scrubber for telemetry: URLs become `<url>`, file system paths (absolute, Windows, or relative with two or
 * more segments) become `<path>`, bundle directory names (`scan_...`) become `<bundle>`. Lives in the library so it is
 * JVM-tested; the field flavour's Telemetry (Sentry) just calls it. Over-scrubbing is acceptable, leaking is not.
 */
object Scrub {
    private const val SEP = """/\\"""

    /** scheme://anything up to whitespace, a quote or a bracket. */
    private val URL = Regex("""[A-Za-z][A-Za-z0-9+.\-]*://[^\s'"()<>\[\]]+""")

    /**
     * Absolute Unix or Windows path. Directory segments may contain spaces ("My Documents"); each starts with a
     * non-space; the last segment stops at whitespace. Must not start in the middle of a word ("1/2", "a/b").
     */
    private val ABS = Regex(
        """(?<![A-Za-z0-9_.\-~])(?:[A-Za-z]:[$SEP]|/)(?:[^\s$SEP:;,'"()\[\]][^\r\n$SEP:;,'"()\[\]]*[$SEP])+[^\s$SEP:;,'"()\[\]]*""",
    )

    /** Relative path candidate: 2+ segments. Kept only if it has a letter or underscore (so "3/4" and "1/2" stay). */
    private val REL = Regex("""[A-Za-z0-9_.\-~]+(?:[$SEP][A-Za-z0-9_.\-~]+)+""")

    private val BUNDLE = Regex("""scan_[A-Za-z0-9_\-]+""")

    fun scrub(s: String): String {
        var t = URL.replace(s, "<url>")
        t = ABS.replace(t, "<path>")
        t = REL.replace(t) { m -> if (m.value.any { it.isLetter() || it == '_' }) "<path>" else m.value }
        return BUNDLE.replace(t, "<bundle>")
    }
}
