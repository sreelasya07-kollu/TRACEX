package dev.capturekit.privacy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class ScrubTest {
    @Test fun unixPathIsReplaced() {
        assertEquals("open failed: <path>", Scrub.scrub("open failed: /data/user/0/dev.capturekit/files/scans/a.jpg"))
    }

    @Test fun windowsPathIsReplacedWithEitherSeparator() {
        assertEquals("x <path> y", Scrub.scrub("""x C:\Users\bob\scan\kf.jpg y"""))
        assertEquals("x <path> y", Scrub.scrub("x D:/Users/bob/scan/kf.jpg y"))
    }

    @Test fun bundleNamesAreReplaced() {
        assertEquals("bundle <bundle> failed", Scrub.scrub("bundle scan_20260101-120000_ab12 failed"))
    }

    @Test fun pathContainingBundleNameLeavesNoName() {
        val out = Scrub.scrub("write /sdcard/Android/data/dev.capturekit/files/scan_2026_01/mcap failed")
        assertFalse(out.contains("scan_2026"))
        assertFalse(out.contains("/sdcard"))
        assertEquals("write <path> failed", out)
    }

    @Test fun relativePathsAreReplaced() {
        assertEquals("cannot read <path> now", Scrub.scrub("cannot read a/b/c.jpg now"))
        assertEquals("cannot read <path>", Scrub.scrub("cannot read keyframes/kf_0001.jpg"))
        assertEquals("x <path>", Scrub.scrub("""x privacy\redaction.json"""))
        assertEquals("write <path> failed", Scrub.scrub("write ./out/scan.mcap failed"))
    }

    @Test fun directoriesWithSpacesAreReplaced() {
        assertEquals("x <path> y", Scrub.scrub("x /storage/emulated/0/My Scans/room 1/kf.jpg y"))
        assertEquals("x <path> y", Scrub.scrub("""x C:\Users\bob smith\Documents\kf.jpg y"""))
        assertEquals("open '<path>' failed", Scrub.scrub("open '/home/my user/a.txt' failed"))
    }

    @Test fun urlsAreReplaced() {
        assertEquals("upload to <url> failed", Scrub.scrub("upload to https://host.example:1080/files/abc123 failed"))
        assertEquals("see <url>", Scrub.scrub("see content://dev.capturekit.files/scans/x.jpg"))
    }

    @Test fun digitOnlySegmentsAreUntouched() {
        assertEquals("ratio 3/4", Scrub.scrub("ratio 3/4"))
        assertEquals("1/2 done", Scrub.scrub("1/2 done"))
        assertEquals("took 12/30 frames, 7 / 9", Scrub.scrub("took 12/30 frames, 7 / 9"))
    }

    @Test fun plainTextIsUntouched() {
        val s = "tracking lost after 3 retries (ratio 1/2, ok)"
        assertEquals(s, Scrub.scrub(s))
        assertEquals("", Scrub.scrub(""))
    }
}
