package dev.capturekit.verdict

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.text.SimpleDateFormat
import java.util.Locale

class ScanInfoTest {
    @Test fun timeComesFromFolderStampElseModifiedTime() {
        val t = ScanInfo.timeMs("scan_20260102_030405", 7L)
        assertEquals(SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).parse("20260102_030405")!!.time, t)
        assertEquals(7L, ScanInfo.timeMs("scan_x", 7L))
        assertEquals(7L, ScanInfo.timeMs("scan_20261399_999999", 7L))
        assertNotEquals(7L, t)
    }

    @Test fun roomLabelIsReadAndUnescaped() {
        assertEquals("Kitchen", ScanInfo.roomLabel("""{"arm":"guided","room":"Kitchen","device":"x"}"""))
        assertEquals("The \"big\" room", ScanInfo.roomLabel("""{"room":"The \"big\" room"}"""))
        assertNull(ScanInfo.roomLabel("""{"room":"  "}""")); assertNull(ScanInfo.roomLabel("{}")); assertNull(ScanInfo.roomLabel(null))
    }

    @Test fun badgeTextAlwaysHasWords() {
        assertEquals("Passed", ScanInfo.badgeText("PASS")); assertEquals("Usable, with notes", ScanInfo.badgeText("WARN"))
        assertEquals("Failed", ScanInfo.badgeText("FAIL")); assertEquals("Not checked", ScanInfo.badgeText(null))
    }
}
