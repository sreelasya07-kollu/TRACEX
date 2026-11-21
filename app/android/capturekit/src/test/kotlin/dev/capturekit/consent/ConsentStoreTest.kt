package dev.capturekit.consent

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ConsentStoreTest {
    private class Mem : ConsentStorage {
        val s = HashMap<String, String>(); val b = HashMap<String, Boolean>()
        override fun getString(key: String) = s[key]
        override fun getBoolean(key: String) = b[key] ?: false
        override fun put(strings: Map<String, String>, booleans: Map<String, Boolean>) { s.putAll(strings); b.putAll(booleans) }
        override fun clear() { s.clear(); b.clear() }
    }

    @Test fun freshStoreHasNoConsentAndNoFlags() {
        val c = ConsentStore(Mem())
        assertFalse(c.hasConsent()); assertFalse(c.uploadAllowed()); assertFalse(c.telemetryAllowed())
        assertNull(c.current()); assertNull(c.sessionConfig())
    }

    @Test fun acceptRecordsVersionTimeAndFlagsSeparately() {
        val c = ConsentStore(Mem())
        c.accept(1_700_000_000_000L, upload = true, telemetry = false)
        val cur = c.current()!!
        assertEquals(ConsentStore.CURRENT_VERSION, cur.version)
        assertEquals("2023-11-14T22:13:20Z", cur.at)
        assertTrue(c.uploadAllowed()); assertFalse(c.telemetryAllowed())
    }

    @Test fun defaultsOffWhenBothTogglesUntouched() {
        val c = ConsentStore(Mem())
        c.accept(0L, upload = false, telemetry = false)
        assertTrue(c.hasConsent()); assertFalse(c.uploadAllowed()); assertFalse(c.telemetryAllowed())
    }

    @Test fun sessionConfigAlwaysCarriesAllThreeKeys() {
        val c = ConsentStore(Mem())
        c.accept(1_700_000_000_000L, upload = false, telemetry = true)
        assertEquals(
            mapOf("consent_version" to "1", "consent_at" to "2023-11-14T22:13:20Z", "redaction_mode" to "faces"),
            c.sessionConfig(),
        )
    }

    @Test fun revokeClearsConsentAndBothFlags() {
        val c = ConsentStore(Mem())
        c.accept(5L, upload = true, telemetry = true)
        c.revoke()
        assertFalse(c.hasConsent()); assertFalse(c.uploadAllowed()); assertFalse(c.telemetryAllowed()); assertNull(c.sessionConfig())
    }

    @Test fun olderTextVersionCountsAsNoConsent() {
        val m = Mem()
        m.put(mapOf("version" to "0", "at" to "2020-01-01T00:00:00Z"), mapOf("upload" to true, "telemetry" to true))
        val c = ConsentStore(m)
        assertFalse(c.hasConsent()); assertFalse(c.uploadAllowed()); assertFalse(c.telemetryAllowed())
    }

    @Test fun flagsWithoutRecordedTimeAreIgnored() {
        val m = Mem()
        m.put(mapOf("version" to ConsentStore.CURRENT_VERSION), mapOf("upload" to true))
        assertFalse(ConsentStore(m).uploadAllowed())
    }

    @Test fun consentTextResourceExistsForCurrentVersion() {
        val f = java.io.File("../sample-app/src/main/res/raw/${ConsentStore.TEXT_RESOURCE}.txt")
        assertTrue("missing ${f.path}", f.isFile)
        assertTrue(f.readText().contains("pending legal owner", ignoreCase = true))
    }

    @Test fun isoIsUtcWithZ() {
        assertEquals("1970-01-01T00:00:00Z", ConsentStore.iso(0L))
    }
}
