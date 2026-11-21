package dev.capturekit.segment

import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

/** What a segmentation prompt points at. Surfaces come from ARCore planes / core layout walls, objects from the core. */
enum class SurfaceKind { FLOOR, WALL, CEILING, OBJECT }

/** Stable identity a prompt carries when it has one (issue #8): the core 3D object id, an ARCore plane, or a core layout wall. */
sealed interface TrackKey {
    /** Core overlay object id (core ObjectLifter, `/guide/` objects). */
    data class CoreObject(val id: Int) : TrackKey
    /** ARCore plane (the host numbers each Plane trackable once per AR session). */
    data class Plane(val id: Long) : TrackKey
    /** Core layout wall id (overlay v3 `surfaces` id, 1-based). */
    data class LayoutWall(val id: Int) : TrackKey
}

/** World point in metres (Y-up, the AR / core world frame). */
data class Vec3(val x: Float, val y: Float, val z: Float) {
    fun dist(o: Vec3): Float { val dx = x - o.x; val dy = y - o.y; val dz = z - o.z; return sqrt(dx * dx + dy * dy + dz * dz) }
}

/** Axis-aligned box in camera-image pixels (x0 < x1, y0 < y1). */
data class Box2(val x0: Float, val y0: Float, val x1: Float, val y1: Float) {
    val area: Float get() = max(0f, x1 - x0) * max(0f, y1 - y0)
    fun iou(o: Box2): Float {
        val iw = min(x1, o.x1) - max(x0, o.x0)
        val ih = min(y1, o.y1) - max(y0, o.y0)
        if (iw <= 0f || ih <= 0f) return 0f
        val inter = iw * ih
        val union = area + o.area - inter
        return if (union > 0f) inter / union else 0f
    }
}

/**
 * One prompt for the segmenter: a positive point ([x], [y] in camera-image pixels; the 8a default prompt), an optional
 * projected box (box prompts are an 8b spike) and the 3D centre used for re-identification.
 */
data class SegPrompt(
    val key: TrackKey?, val kind: SurfaceKind, val label: String,
    val x: Float, val y: Float, val box: Box2? = null, val centre: Vec3? = null,
)

/** A remembered object or surface: [id] never changes and is never reused; [color] is ARGB, a function of [id]. */
data class Track(val id: Int, val kind: SurfaceKind, val label: String, val color: Int)

/**
 * Identity memory for the segmentation overlay (issue #8). It lives in app code, not in the model: the on-device SAM 2
 * exports have no memory encoder. A prompt with a [TrackKey] keeps its track for as long as the key is remembered; a
 * prompt without one (or with a key not seen yet) is matched to a remembered track of the same kind and label by 3D
 * centre distance <= [maxCentreDistM] or projected-box IoU >= [minIou]. A keyed prompt never takes a track that holds a
 * different key (two chairs side by side stay two tracks). Bounded LRU of [capacity] tracks, so an object that leaves
 * the view and comes back keeps its id and colour unless [capacity] newer tracks were seen meanwhile.
 * Not thread-safe: the segmentation worker thread owns it.
 */
class TrackMemory(
    private val capacity: Int = 48,
    private val minIou: Float = 0.3f,          // guess; tune on device recordings (8b)
    private val maxCentreDistM: Float = 0.5f,  // guess; tune on device recordings (8b)
) {
    private class Entry(val id: Int, val kind: SurfaceKind, var label: String, var key: TrackKey?, var box: Box2?, var centre: Vec3?)

    private val byId = LinkedHashMap<Int, Entry>(16, 0.75f, true)   // access order: first = least recently seen
    private val byKey = HashMap<TrackKey, Entry>()
    private var nextId = 1

    init { require(capacity >= 1) }

    val size: Int get() = byId.size

    /** One run's prompts -> one track each, same order. A track goes to at most one prompt per run. */
    fun assign(prompts: List<SegPrompt>): List<Track> {
        val out = arrayOfNulls<Entry>(prompts.size)
        val used = HashSet<Int>()
        // 1) Known keys.
        for ((i, p) in prompts.withIndex()) {
            val e = p.key?.let { byKey[it] } ?: continue
            if (e.id in used) continue
            out[i] = e; used.add(e.id)
        }
        // 2) Re-identification by 3D centre / projected box among same kind + label.
        for ((i, p) in prompts.withIndex()) {
            if (out[i] != null) continue
            var best: Entry? = null
            var bestScore = 0f
            for (e in byId.values) {
                if (e.id in used || e.kind != p.kind || e.label != p.label) continue
                if (p.key != null && e.key != null && e.key != p.key) continue
                val s = matchScore(p, e)
                if (s > bestScore) { bestScore = s; best = e }
            }
            if (best != null) {
                out[i] = best; used.add(best.id)
                if (p.key != null && best.key == null) { best.key = p.key; byKey[p.key] = best }
            }
        }
        // 3) New tracks.
        for ((i, p) in prompts.withIndex()) {
            if (out[i] != null) continue
            val e = Entry(nextId++, p.kind, p.label, p.key, null, null)
            byId[e.id] = e
            if (p.key != null) byKey[p.key] = e
            out[i] = e; used.add(e.id)
        }
        // Update what was seen (also refreshes the LRU order), then trim the oldest tracks not used in this run.
        for ((i, p) in prompts.withIndex()) {
            val e = byId.getValue(out[i]!!.id)
            e.label = p.label
            if (p.box != null) e.box = p.box
            if (p.centre != null) e.centre = p.centre
        }
        if (byId.size > capacity) {
            val it = byId.values.iterator()
            while (byId.size > capacity && it.hasNext()) {
                val e = it.next()
                if (e.id in used) continue
                it.remove()
                e.key?.let { k -> if (byKey[k] === e) byKey.remove(k) }
            }
        }
        return out.map { e -> Track(e!!.id, e.kind, e.label, colorFor(e.id)) }
    }

    /**
     * > 0 when [p] may be [e]. With both 3D centres known the centre decides (the stored box is from another viewpoint
     * and only breaks ties); otherwise the projected-box IoU decides. 0 = no match.
     */
    private fun matchScore(p: SegPrompt, e: Entry): Float {
        val pb = p.box; val eb = e.box
        val iou = if (pb != null && eb != null) pb.iou(eb) else 0f
        val pc = p.centre; val ec = e.centre
        if (pc != null && ec != null) {
            val d = pc.dist(ec)
            return if (d <= maxCentreDistM) 1f + (1f - d / maxCentreDistM) + iou else 0f
        }
        return if (iou >= minIou) iou else 0f
    }

    fun clear() { byId.clear(); byKey.clear() }

    companion object {
        /** 12 well-separated hues (opaque ARGB); the overlay applies its own alpha. */
        private val PALETTE = intArrayOf(
            0xFF42A5F5.toInt(), 0xFFFFA726.toInt(), 0xFF66BB6A.toInt(), 0xFFEC407A.toInt(),
            0xFFAB47BC.toInt(), 0xFF26C6DA.toInt(), 0xFFFFEE58.toInt(), 0xFF8D6E63.toInt(),
            0xFF5C6BC0.toInt(), 0xFFD4E157.toInt(), 0xFFEF5350.toInt(), 0xFF26A69A.toInt(),
        )

        /** Stable colour of track [id] (1-based). */
        fun colorFor(id: Int): Int = PALETTE[Math.floorMod(id - 1, PALETTE.size)]
    }
}
