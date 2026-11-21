package dev.capturekit.guidance

import dev.capturekit.capture.LabelMap
import dev.capturekit.capture.ObjectBox
import dev.capturekit.capture.Opening

/**
 * "What was found in the room" for the review screens and TalkBack (pure, JVM-tested): lifted objects counted per class
 * plus the doors and windows the core found in the walls, e.g. "2 chairs, 1 sofa, 1 TV, 1 door". Words come from
 * [LabelMap.displayName] (same words as the core's spoken landmarks).
 */
object ObjectSummary {
    /** One line of the summary: [name] is the core class name (or "door" / "window"), [n] >= 1. */
    data class Count(val name: String, val n: Int) {
        val text: String get() = "$n ${LabelMap.displayName(name, plural = n != 1)}"
    }

    /**
     * Counts, most frequent first; ties keep the order in which each kind first appears (objects before openings), so
     * the result is stable for a given snapshot. Blank / "unknown" class names and openings of another kind are skipped.
     */
    fun counts(objects: List<ObjectBox>, openings: List<Opening> = emptyList()): List<Count> {
        val n = LinkedHashMap<String, Int>()
        for (o in objects) if (o.cls.isNotBlank() && o.cls != "unknown") n[o.cls] = (n[o.cls] ?: 0) + 1
        for (o in openings) if (o.kind == "door" || o.kind == "window") n[o.kind] = (n[o.kind] ?: 0) + 1
        return n.entries.map { Count(it.key, it.value) }.sortedByDescending { it.n }   // stable sort: first-seen order on ties
    }

    /** "2 chairs, 1 sofa, 1 door"; "" when nothing was found. */
    fun text(objects: List<ObjectBox>, openings: List<Opening> = emptyList()): String =
        counts(objects, openings).joinToString(", ") { it.text }
}
