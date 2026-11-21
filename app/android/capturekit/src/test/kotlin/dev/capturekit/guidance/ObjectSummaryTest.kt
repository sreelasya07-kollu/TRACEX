package dev.capturekit.guidance

import dev.capturekit.capture.EventParser
import dev.capturekit.capture.ObjectBox
import dev.capturekit.capture.Opening
import org.junit.Assert.assertEquals
import org.junit.Test

class ObjectSummaryTest {
    private fun obj(id: Int, cls: String) = ObjectBox(id, cls, 0.0, 0.0, 0.5, 0.5, 0, 0.9)
    private fun open(kind: String) = Opening(kind, "W1", 0.0, 0.0, 1.0, 0.0, 0.0, 2.0)

    @Test fun nothingFoundIsEmpty() {
        assertEquals("", ObjectSummary.text(emptyList()))
        assertEquals(emptyList<ObjectSummary.Count>(), ObjectSummary.counts(emptyList(), emptyList()))
    }

    @Test fun countsPerClassMostFrequentFirstWithSpokenWordsAndPlurals() {
        val objects = listOf(obj(1, "couch"), obj(2, "chair"), obj(3, "tv"), obj(4, "chair"), obj(5, "dining_table"))
        assertEquals("2 chairs, 1 sofa, 1 TV, 1 table", ObjectSummary.text(objects))
    }

    @Test fun doorsAndWindowsFromTheWallsAreIncludedAfterObjectsOnTies() {
        val s = ObjectSummary.text(listOf(obj(1, "bed")), listOf(open("window"), open("door"), open("window"), open("skylight")))
        assertEquals("2 windows, 1 bed, 1 door", s)
    }

    @Test fun unknownAndBlankClassesAreSkippedButNewClassesAreKept() {
        val s = ObjectSummary.text(listOf(obj(1, "unknown"), obj(2, ""), obj(3, "refrigerator"), obj(4, "coffee_machine")))
        assertEquals("1 fridge, 1 coffee machine", s)
    }

    @Test fun worksOnTheOverlayStringTheReviewScreenReceives() {
        // The review hand-over encodes the last overlay's objects; the summary must read the same parse result.
        val encoded = EventParser.encodeObjects(listOf(obj(1, "chair"), obj(2, "shelf"), obj(3, "shelf")))
        assertEquals("2 shelves, 1 chair", ObjectSummary.text(EventParser.parseObjects(encoded)))
    }
}
