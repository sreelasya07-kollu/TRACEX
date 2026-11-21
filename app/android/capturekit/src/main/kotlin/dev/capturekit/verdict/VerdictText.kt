package dev.capturekit.verdict

/**
 * Plain-language text for the validator's verdict and issue codes, from assets/verdict_messages.json
 * ({"headlines":{FAIL,WARN,PASS},"codes":{CODE:{severity,level,title,text,action,next}}}). Pure: the JSON is parsed
 * by a tiny reader here because org.json is stubbed in JVM unit tests. Unknown codes get a generic entry.
 */
class VerdictText(private val headlines: Map<String, String>, private val codes: Map<String, Entry>) {
    data class Entry(val severity: String, val level: String, val title: String, val text: String, val action: String, val next: String)
    data class Issue(val code: String, val severity: String, val message: String)

    fun headline(verdict: String?): String = headlines[verdict] ?: headlines["WARN"] ?: "The scan check is not available."

    fun entry(issue: Issue): Entry = codes[issue.code] ?: Entry(
        issue.severity, "semantic", "Scan note",
        issue.message.ifEmpty { "The check reported a note about this scan." },
        if (issue.severity == "ERROR") "Scan the room again." else "Nothing needed.",
        if (issue.severity == "ERROR") "rescan" else "proceed_with_note",
    )

    /** WARN/ERROR issues only, one entry per code, errors first (stable otherwise). */
    fun describe(issues: List<Issue>): List<Entry> {
        val seen = HashSet<String>()
        return issues.filter { (it.severity == "ERROR" || it.severity == "WARN") && seen.add(it.code) }
            .sortedBy { if (it.severity == "ERROR") 0 else 1 }.map { entry(it) }
    }

    /** "rescan" if any entry says so or the verdict is FAIL, else "proceed_with_note" if any note, else "none". */
    fun nextStep(verdict: String?, issues: List<Issue>): String {
        val e = describe(issues)
        return when {
            verdict == "FAIL" || e.any { it.next == "rescan" } -> "rescan"
            e.any { it.next == "proceed_with_note" } || verdict == "WARN" -> "proceed_with_note"
            else -> "none"
        }
    }

    companion object {
        val FALLBACK = VerdictText(
            mapOf("FAIL" to "This scan cannot be used. Please scan the room again.", "WARN" to "Scan saved with notes.", "PASS" to "Scan looks good."),
            emptyMap(),
        )

        /** Never throws: a damaged asset yields [FALLBACK]. */
        @Suppress("UNCHECKED_CAST")
        fun parse(json: String): VerdictText = try {
            val root = MiniJson(json).value() as Map<String, Any?>
            val heads = (root["headlines"] as? Map<String, Any?>).orEmpty().mapNotNull { (k, v) -> (v as? String)?.let { k to it } }.toMap()
            val codes = LinkedHashMap<String, Entry>()
            ((root["codes"] as? Map<String, Any?>).orEmpty()).forEach { (k, v) ->
                val m = v as? Map<String, Any?> ?: return@forEach
                fun s(n: String) = m[n] as? String ?: ""
                codes[k] = Entry(s("severity"), s("level"), s("title"), s("text"), s("action"), s("next"))
            }
            if (heads.isEmpty() && codes.isEmpty()) FALLBACK else VerdictText(FALLBACK.headlines + heads, codes)
        } catch (t: Throwable) { FALLBACK }

        /** Issues of manifest.json validation.issues ({"code","severity","message"} objects). */
        @Suppress("UNCHECKED_CAST")
        fun issuesOf(manifestJson: String): List<Issue> = try {
            val root = MiniJson(manifestJson).value() as Map<String, Any?>
            ((root["validation"] as? Map<String, Any?>)?.get("issues") as? List<Any?>).orEmpty().mapNotNull { o ->
                val m = o as? Map<*, *> ?: return@mapNotNull null
                Issue(m["code"] as? String ?: "", m["severity"] as? String ?: "", m["message"] as? String ?: "")
            }
        } catch (t: Throwable) { emptyList() }
    }
}

/** Minimal JSON reader: objects -> LinkedHashMap, arrays -> List, strings, numbers (Double), true/false/null. */
internal class MiniJson(private val s: String) {
    private var i = 0

    fun value(): Any? = read()

    private fun ws() { while (i < s.length && s[i].isWhitespace()) i++ }
    private fun fail(): Nothing = throw IllegalArgumentException("bad json at $i")

    private fun read(): Any? {
        ws()
        if (i >= s.length) fail()
        val c = s[i]
        return when {
            c == '{' -> obj()
            c == '[' -> arr()
            c == '"' -> str()
            c == 't' -> lit("true", true)
            c == 'f' -> lit("false", false)
            c == 'n' -> lit("null", null)
            c == '-' || c.isDigit() -> num()
            else -> fail()
        }
    }

    private fun lit(w: String, v: Any?): Any? { if (!s.startsWith(w, i)) fail(); i += w.length; return v }

    private fun num(): Double {
        val st = i
        while (i < s.length && (s[i].isDigit() || s[i] in "+-.eE")) i++
        return s.substring(st, i).toDoubleOrNull() ?: fail()
    }

    private fun obj(): Map<String, Any?> {
        val m = LinkedHashMap<String, Any?>()
        i++; ws()
        if (i < s.length && s[i] == '}') { i++; return m }
        while (true) {
            ws(); if (i >= s.length || s[i] != '"') fail()
            val k = str(); ws()
            if (i >= s.length || s[i] != ':') fail()
            i++; m[k] = read(); ws()
            if (i >= s.length) fail()
            if (s[i] == ',') { i++; continue }
            if (s[i] == '}') { i++; return m }
            fail()
        }
    }

    private fun arr(): List<Any?> {
        val l = ArrayList<Any?>()
        i++; ws()
        if (i < s.length && s[i] == ']') { i++; return l }
        while (true) {
            l.add(read()); ws()
            if (i >= s.length) fail()
            if (s[i] == ',') { i++; continue }
            if (s[i] == ']') { i++; return l }
            fail()
        }
    }

    private fun str(): String {
        i++
        val b = StringBuilder()
        while (true) {
            if (i >= s.length) fail()
            val c = s[i++]
            if (c == '"') return b.toString()
            if (c != '\\') { b.append(c); continue }
            if (i >= s.length) fail()
            when (s[i++]) {
                'n' -> b.append('\n')
                't' -> b.append('\t')
                'r' -> b.append('\r')
                'b' -> b.append('\b')
                'f' -> b.append('\u000c')
                '/' -> b.append('/')
                '\\' -> b.append('\\')
                '"' -> b.append('"')
                'u' -> { if (i + 4 > s.length) fail(); b.append(s.substring(i, i + 4).toInt(16).toChar()); i += 4 }
                else -> fail()
            }
        }
    }
}
