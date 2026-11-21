package dev.capturekit.capture

/**
 * Claim/publish/pin bookkeeping for the keyframe ring (pure JVM; the plane bytes live in SessionManager).
 * Writer (AR thread): [claim] an index (its id is invalidated, so it cannot be pinned while written), then
 * [publish] it on success; on failure simply do not publish, and the same slot is claimed next time.
 * Reader (ck-jpeg): [pin] by frame id, read, [unpin]. A pinned slot is never claimed. All calls are short and locked.
 */
class SlotRing(val size: Int) {
    init { require(size > 0) }
    private val ids = IntArray(size) { -1 }
    private val pinned = BooleanArray(size)
    private var next = 0

    /** Next unpinned index in ring order (oldest content evicted), with its id cleared; null if every slot is pinned. */
    @Synchronized fun claim(): Int? {
        for (k in 0 until size) {
            val i = (next + k) % size
            if (!pinned[i]) { ids[i] = -1; return i }
        }
        return null
    }

    /** Makes [index] readable as [id]; the next claim starts after it. Call after the slot's data is written. */
    @Synchronized fun publish(index: Int, id: Int) { ids[index] = id; next = (index + 1) % size }

    /** Pins the slot holding [id] and returns its index, or null if it was evicted / never published. */
    @Synchronized fun pin(id: Int): Int? {
        if (id < 0) return null
        for (i in 0 until size) if (ids[i] == id) { pinned[i] = true; return i }
        return null
    }

    @Synchronized fun unpin(index: Int) { pinned[index] = false }
}
