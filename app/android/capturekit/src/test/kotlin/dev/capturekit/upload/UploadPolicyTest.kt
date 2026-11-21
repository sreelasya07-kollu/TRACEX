package dev.capturekit.upload

import androidx.work.WorkInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class UploadPolicyTest {
    @Test fun runsOnlyWithConsentNowAndNonFailVerdict() {
        assertEquals(UploadPolicy.Decision.RUN, UploadPolicy.decide(true, "PASS"))
        assertEquals(UploadPolicy.Decision.RUN, UploadPolicy.decide(true, "WARN"))
        assertEquals(UploadPolicy.Decision.NO_VERDICT, UploadPolicy.decide(true, null))
        assertEquals(UploadPolicy.Decision.NO_VERDICT, UploadPolicy.decide(true, "MAYBE"))
        assertFalse(UploadPolicy.eligible(true, null)); assertFalse(UploadPolicy.eligible(true, "FAIL")); assertTrue(UploadPolicy.eligible(true, "WARN"))
        assertEquals(UploadPolicy.Decision.VERDICT_FAIL, UploadPolicy.decide(true, "FAIL"))
    }

    @Test fun revokedConsentStopsQueuedWorkWhateverTheVerdict() {
        for (v in listOf("PASS", "WARN", "FAIL", null)) {
            assertEquals(UploadPolicy.Decision.NO_CONSENT, UploadPolicy.decide(false, v))
            assertFalse(UploadPolicy.eligible(false, v))
        }
        assertTrue(UploadPolicy.eligible(true, "PASS"))
    }

    @Test fun verdictOfReadsManifest() {
        assertEquals("FAIL", UploadPolicy.verdictOf("""{"validation":{"verdict": "FAIL","issues":[]}}"""))
        assertNull(UploadPolicy.verdictOf("""{"validation":{"verdict":"MAYBE"}}"""))
        assertNull(UploadPolicy.verdictOf("garbage"))
    }

    @Test fun workStatesMapToUserStates() {
        assertEquals(UploadState.WAITING, UploadState.fromWork(WorkInfo.State.ENQUEUED, 0))
        assertEquals(UploadState.RETRYING, UploadState.fromWork(WorkInfo.State.ENQUEUED, 2))
        assertEquals(UploadState.WAITING, UploadState.fromWork(WorkInfo.State.BLOCKED, 0))
        assertEquals(UploadState.UPLOADING, UploadState.fromWork(WorkInfo.State.RUNNING, 0))
        assertEquals(UploadState.VERIFIED, UploadState.fromWork(WorkInfo.State.SUCCEEDED, 1))
        assertEquals(UploadState.FAILED, UploadState.fromWork(WorkInfo.State.FAILED, 0))
        assertEquals(UploadState.FAILED, UploadState.fromWork(WorkInfo.State.CANCELLED, 0))
    }

    @Test fun percentIsClamped() {
        assertEquals(0, UploadPolicy.percent(5, 0)); assertEquals(50, UploadPolicy.percent(50, 100)); assertEquals(100, UploadPolicy.percent(300, 100))
    }

    @Test fun markerSitsBesideTheBundleAndIsRecognised() {
        val m = UploadPolicy.verifiedMarker(File("/x/bundles/scan_1"))
        assertEquals("scan_1.upload_verified", m.name); assertEquals("bundles", m.parentFile!!.name)
        assertTrue(UploadPolicy.isMarker(m.name)); assertFalse(UploadPolicy.isMarker("scan_1"))
    }
}
