package dev.capturekit.platform

import org.junit.Assert.assertEquals
import org.junit.Test

class ThermalMappingTest {
    @Test
    fun thermalStatusTable() {
        val expected = mapOf(0 to 0, 1 to 1, 2 to 1, 3 to 2, 4 to 3, 5 to 3, 6 to 3)
        expected.forEach { (status, level) -> assertEquals("status $status", level, ThermalMapping.fromThermalStatus(status)) }
    }

    @Test
    fun headroomEscalatesEarly() {
        assertEquals(0, ThermalMapping.fromHeadroom(Float.NaN))
        assertEquals(0, ThermalMapping.fromHeadroom(0.5f))
        assertEquals(1, ThermalMapping.fromHeadroom(0.9f))
        assertEquals(2, ThermalMapping.fromHeadroom(1.05f))
        assertEquals(2, ThermalMapping.combine(1, 1.2f))
        assertEquals(3, ThermalMapping.combine(4, 0.1f))
    }
}
