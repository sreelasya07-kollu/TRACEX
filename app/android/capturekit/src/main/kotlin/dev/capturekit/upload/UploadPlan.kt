package dev.capturekit.upload

import java.io.File
import java.security.MessageDigest

/** Pure helpers (JVM-testable): upload order and the verifier's status JSON. */
object UploadPlan {
    const val MANIFEST = "manifest.json"

    /** Every file of the bundle, relative paths with '/', manifest.json LAST (server verifies on its arrival). */
    fun order(bundle: File): List<String> {
        val all = bundle.walkTopDown().filter { it.isFile && !it.name.endsWith(".tmp") && !it.name.startsWith(".ckit_upload") }
            .map { it.relativeTo(bundle).invariantSeparatorsPath }.sorted().toList()
        return all.filter { it != MANIFEST } + all.filter { it == MANIFEST }
    }

    fun sha256(f: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        f.inputStream().use { s -> val b = ByteArray(1 shl 16); while (true) { val n = s.read(b); if (n < 0) break; md.update(b, 0, n) } }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    fun bundleId(manifestJson: String): String? = Regex("\"bundle_id\"\\s*:\\s*\"([^\"]+)\"").find(manifestJson)?.groupValues?.get(1)
}

/** GET /bundles/<id> on the verifier. */
data class ServerStatus(val status: String, val reason: String, val corruptPaths: List<String>, val missing: List<String>) {
    val isFinal get() = status == "COMPLETE" || status == "CORRUPT"

    companion object {
        fun parse(json: String): ServerStatus {
            fun str(k: String) = Regex("\"$k\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"").find(json)?.groupValues?.get(1) ?: ""
            fun arr(k: String) = Regex("\"$k\"\\s*:\\s*\\[([^\\]]*)]").find(json)?.groupValues?.get(1)
                ?.let { Regex("\"((?:[^\"\\\\]|\\\\.)*)\"").findAll(it).map { m -> m.groupValues[1] }.toList() } ?: emptyList()
            val corrupt = Regex("\"path\"\\s*:\\s*\"([^\"]+)\"").findAll(
                Regex("\"corrupt\"\\s*:\\s*\\[(.*?)]\\s*,\\s*\"(?:missing|reason|received|status|verdict)\"", RegexOption.DOT_MATCHES_ALL)
                    .find(json)?.groupValues?.get(1) ?: Regex("\"corrupt\"\\s*:\\s*\\[(.*?)]", RegexOption.DOT_MATCHES_ALL).find(json)?.groupValues?.get(1) ?: "",
            ).map { it.groupValues[1] }.toList()
            return ServerStatus(str("status"), str("reason"), corrupt, arr("missing"))
        }
    }
}
