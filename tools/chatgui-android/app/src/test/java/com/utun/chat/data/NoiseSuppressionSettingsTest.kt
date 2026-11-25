package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class NoiseSuppressionSettingsTest {
    @Test fun enabledByDefaultAndBypassIsIndependentOfSavedStrength() {
        val defaults = NoiseSuppressionSettings()
        assertTrue(defaults.enabled)
        assertEquals(-20, defaults.nativeLevel)
        val disabled = defaults.copy(enabled = false, suppressDb = -35)
        assertEquals(0, disabled.nativeLevel)
        assertEquals(-35, disabled.copy(enabled = true).nativeLevel)
    }

    @Test fun everySliderValueHasAnExactNativeRepresentation() {
        for (db in -40..-1) assertEquals(db, NoiseSuppressionSettings(suppressDb = db).nativeLevel)
        for (db in listOf(Int.MIN_VALUE, -41, 0, 1, Int.MAX_VALUE)) {
            try { NoiseSuppressionSettings(suppressDb = db); fail("Accepted invalid NS value: $db") }
            catch (_: IllegalArgumentException) { }
        }
    }
}
