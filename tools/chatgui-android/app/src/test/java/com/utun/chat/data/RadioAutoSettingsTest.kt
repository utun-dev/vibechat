package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class RadioAutoSettingsTest {
    @Test fun validatesWholeThresholdPairAndBothModes() {
        for (mode in listOf(RadioAutoSettings.VAD, RadioAutoSettings.LEVEL, RadioAutoSettings.VAD_LEVEL)) {
            val settings = RadioAutoSettings(mode = mode)
            assertTrue(settings.valid())
            assertTrue(settings.copy(levelOn = 0, levelOff = -90).valid())
            assertFalse(settings.copy(levelOn = -45, levelOff = -45).valid())
            assertFalse(settings.copy(levelOn = -50, levelOff = -40).valid())
            assertFalse(settings.copy(levelOff = -91).valid())
            assertFalse(settings.copy(levelOn = 1).valid())
            assertFalse(settings.copy(hangoverMs = 49).valid())
            assertFalse(settings.copy(hangoverMs = 2001).valid())
            assertFalse(settings.copy(vadThreshold = 96).valid())
            assertFalse(settings.copy(vadGain = 0).valid())
        }
        assertFalse(RadioAutoSettings(mode = RadioAutoSettings.OFF).valid())
        assertFalse(RadioAutoSettings(mode = 4).valid())
    }

    @Test fun nativeSnapshotHasExplicitModeAndSignedThresholds() {
        val settings = RadioAutoSettings(true, RadioAutoSettings.LEVEL, -40, -60, 500, 70, 20)
        assertArrayEquals(intArrayOf(1, 2, -40, -60, 500, 70, 20), settings.nativeValues())
        assertEquals(0, settings.copy(enabled = false).nativeValues()[0])
    }

    @Test fun monitorNeverEnablesTransmissionAndReleasesCaptureWhenHidden() {
        for (mode in listOf(RadioAutoSettings.VAD, RadioAutoSettings.LEVEL, RadioAutoSettings.VAD_LEVEL)) {
            val settings = RadioAutoSettings(mode = mode)
            assertNull(radioCaptureMode(true, true, true, settings, manual = false, monitor = false))
            assertEquals(RadioAutoSettings.OFF, radioCaptureMode(true, true, true, settings, manual = false, monitor = true))
            assertNull(radioCaptureMode(true, false, true, settings, manual = false, monitor = true))
            assertNull(radioCaptureMode(false, true, true, settings, manual = true, monitor = true))
        }
    }

    @Test fun manualBypassesBackgroundWindowButNeverScreenOff() {
        for (mode in listOf(RadioAutoSettings.VAD, RadioAutoSettings.LEVEL, RadioAutoSettings.VAD_LEVEL)) {
            val settings = RadioAutoSettings(enabled = true, mode = mode)
            assertEquals(mode, radioCaptureMode(true, true, true, settings, manual = false, monitor = false))
            assertNull(radioCaptureMode(true, true, false, settings, manual = false, monitor = false))
            assertNull(radioCaptureMode(true, false, true, settings, manual = false, monitor = false))
            assertNull(radioCaptureMode(true, false, false, settings, manual = true, monitor = false))
            assertEquals(RadioAutoSettings.OFF, radioCaptureMode(true, true, false, settings, manual = false, monitor = true))
            assertEquals(mode, radioCaptureMode(true, true, true, settings, manual = true, monitor = true))
        }
    }

    @Test fun standbyKeepsOnlyCaptureAndRequiresAnActiveScreen() {
        val settings = RadioAutoSettings()
        assertEquals(RadioAutoSettings.OFF, radioCaptureMode(true, true, false, settings, false, false, hold = true))
        assertNull(radioCaptureMode(true, false, false, settings, true, true, hold = true))
        assertNull(radioCaptureMode(false, true, false, settings, true, true, hold = true))
    }

    @Test fun foregroundIsUnlimitedAndEveryBackgroundPeriodGetsNewWindow() {
        val duration = 15 * 60 * 1000L
        val window = RadioVadWindow(duration)
        window.setForeground(true, 100)
        window.setEnabled(true, 100)
        assertTrue(window.isAllowed(2 * 60 * 60 * 1000L))
        window.setForeground(false, 8_000_000)
        assertTrue(window.isAllowed(8_000_000 + duration - 1))
        assertFalse(window.isAllowed(8_000_000 + duration))
        window.setForeground(true, 9_000_000)
        window.setForeground(false, 10_000_000)
        assertEquals(duration, window.timeLeftMs(10_000_000))
        window.close()
        assertFalse(window.isAllowed(10_000_001))
        window.restart(10_000_100)
        assertTrue(window.isAllowed(10_000_101))
    }
}
