package dev.capturekit

import org.junit.Assert.assertEquals
import org.junit.Test

class CaptureKitTest {
    @Test
    fun version() {
        assertEquals("0.0.1", CaptureKit.VERSION)
    }
}
