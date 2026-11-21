package dev.capturekit.guidance

import dev.capturekit.capture.CoreEvent

/** One wall as the user knows it: "Wall N" (N = stable overlay wall id, ADR 0022; list position for a v1 core) and % seen. */
data class WallState(val id: Int, val pct: Int) {
    val name: String get() = "Wall $id"
}

/**
 * What the app currently knows about the scan, as plain data: the only input of [StateAnswers] and the only
 * thing a future dialog proxy may be shown (capturekit/docs/dialog-proxy.md). No image, pixel, path, position
 * or timestamp is carried. Built from the latest [CoreEvent.Overlay]; [labels] and [gaps] stay empty until the
 * understanding layer (Phase 9, flag off) supplies them.
 */
data class ScanStateSnapshot(
    /** 0..1 sufficient fraction of valid plane cells (overlay `completeness`). */
    val completeness: Double,
    /** Overlay `tracking`: 1 normal, 2 limited, 3 not available. */
    val tracking: Int,
    val wallFound: Boolean,
    val floorFound: Boolean,
    val ceilingFound: Boolean,
    val walls: List<WallState>,
    /** Number of 30-degree heading sectors (of 12) a keyframe has looked along. */
    val headingSectors: Int,
    /** Object labels seen ("door", "window", ...). */
    val labels: List<String> = emptyList(),
    /** Named areas still missing, already worded by the producer ("Window area behind you"). Cited as is. */
    val gaps: List<String> = emptyList(),
    /** Walls found (overlay `walls` from a v3 core: layout walls with evidence); defaults to the list size. */
    val wallCount: Int = walls.size,
) {
    /** False until the core has reported anything about the room: answers must abstain. */
    val hasScanData: Boolean
        get() = wallFound || floorFound || ceilingFound || walls.isNotEmpty() || headingSectors > 0 || completeness > 0.0

    /** Every region name an answer may cite. */
    val regions: Set<String>
        get() = buildSet {
            walls.forEach { add(it.name) }
            if (floorFound) add(FLOOR)
            if (ceilingFound) add(CEILING)
            addAll(gaps)
        }

    /** Sorted-key flat JSON for the dialog-proxy request ("state" object). Built by hand: JVM-testable, no org.json. */
    fun toJson(): String = buildString {
        append('{')
        append("\"ceiling_found\":").append(ceilingFound)
        append(",\"completeness\":").append(round2(completeness))
        append(",\"floor_found\":").append(floorFound)
        append(",\"gaps\":").append(strings(gaps))
        append(",\"heading_sectors\":").append(headingSectors)
        append(",\"labels\":").append(strings(labels))
        append(",\"tracking\":").append(tracking)
        append(",\"wall_found\":").append(wallFound)
        append(",\"walls\":[")
        walls.forEachIndexed { i, w -> if (i > 0) append(','); append("{\"id\":").append(w.id).append(",\"pct\":").append(w.pct).append('}') }
        append("]}")
    }

    companion object {
        const val FLOOR = "Floor"
        const val CEILING = "Ceiling"

        /**
         * Walls the user can still be asked about. A v3 core (layout walls): only walls with a real id (> 0; plane-less
         * inferred walls and hypothesised outline sides parse as id 0) whose spans are not ALL blocked by furniture.
         * An older core: every surface, as before.
         */
        fun evidencedWalls(o: CoreEvent.Overlay): List<dev.capturekit.capture.WallSeg> =
            if (!o.layoutV3) o.surfaces else o.surfaces.filter { it.id > 0 && !allSpansBlocked(o, it.id) }

        private fun allSpansBlocked(o: CoreEvent.Overlay, id: Int): Boolean {
            var n = 0
            for (s in o.spans) if (s.id == id) { n++; if (s.state != 'b') return false }
            return n > 0
        }

        fun fromOverlay(o: CoreEvent.Overlay, labels: List<String> = emptyList(), gaps: List<String> = emptyList()): ScanStateSnapshot {
            // v3: names come straight from the core ids (stable across plane merges); never renumbered.
            // v1 core: walls without an id get ids after the largest real id, so they never collide with real ones.
            var nextId = (o.surfaces.maxOfOrNull { it.id } ?: 0).coerceAtLeast(0)
            val walls = if (o.layoutV3) evidencedWalls(o).map { WallState(it.id, it.pct.coerceIn(0, 100)) }
            else o.surfaces.map { s -> WallState(if (s.id > 0) s.id else ++nextId, s.pct.coerceIn(0, 100)) }
            return ScanStateSnapshot(
                completeness = finite01(o.completeness),
                tracking = o.tracking,
                wallFound = o.wallFound,
                floorFound = o.floorFound,
                ceilingFound = o.ceilingFound || o.ceilingSeen,
                walls = walls,
                headingSectors = Integer.bitCount(o.yawMask and 0xFFF),
                labels = labels,
                gaps = gaps,
                wallCount = if (o.layoutV3) o.walls else walls.size,
            )
        }

        /** NaN and infinities become 0.0 (coerceIn passes NaN through). */
        private fun finite01(d: Double): Double = if (d.isNaN() || d.isInfinite()) 0.0 else d.coerceIn(0.0, 1.0)

        private fun round2(d: Double): String = (Math.round(finite01(d) * 100.0) / 100.0).toString()

        private fun strings(l: List<String>): String = l.joinToString(",", "[", "]") { "\"" + escape(it) + "\"" }

        private fun escape(s: String): String = buildString {
            for (c in s) when {
                c == '\\' -> append("\\\\")
                c == '"' -> append("\\\"")
                c == '\n' -> append("\\n")
                c == '\r' -> append("\\r")
                c == '\t' -> append("\\t")
                c.code < 0x20 -> append("\\u%04x".format(c.code))
                else -> append(c)
            }
        }
    }
}
