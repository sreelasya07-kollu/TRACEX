package dev.capturekit.capture

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SlotRingTest {
    @Test fun pinnedSlotIsNeverClaimed() {
        val r = SlotRing(3)
        for (id in 0 until 3) r.publish(r.claim()!!, id)   // slots 0,1,2 hold ids 0,1,2
        assertEquals(0, r.pin(0))
        val claimed = (0 until 6).map { r.claim()!!.also { i -> r.publish(i, 100 + it) } }
        assertEquals(false, claimed.contains(0))
        assertEquals(0, r.pin(0))                          // still readable, untouched
        r.unpin(0)
    }

    @Test fun allSlotsPinnedGivesNull() {
        val r = SlotRing(2)
        r.publish(r.claim()!!, 7); r.publish(r.claim()!!, 8)
        r.pin(7); r.pin(8)
        assertNull(r.claim())
        r.unpin(1)
        assertEquals(1, r.claim())
    }

    @Test fun failedIngestReusesSameSlotAndInvalidatesIt() {
        val r = SlotRing(4)
        r.publish(r.claim()!!, 1)
        val a = r.claim()!!                                 // ingest fails: not published
        assertEquals(a, r.claim())
        assertNull(r.pin(-1))
    }

    @Test fun oldestIsEvictedAndClaimInvalidatesId() {
        val r = SlotRing(2)
        r.publish(r.claim()!!, 1); r.publish(r.claim()!!, 2)
        assertEquals(0, r.claim())                          // wraps to the oldest
        assertNull(r.pin(1))                                // being rewritten: cannot be pinned
        assertEquals(1, r.pin(2))
    }
}
