package com.utun.chat.data

import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.mutablePreferencesOf
import androidx.datastore.preferences.core.stringPreferencesKey
import org.junit.Assert.*
import org.junit.Test

class MonitoringSettingsTest {
    private val enabled = booleanPreferencesKey("control_enabled")
    private val ip = stringPreferencesKey("control_bind_ip")
    private val port = intPreferencesKey("control_port")
    private fun config(settings: MonitoringSettings) = buildString { settings.appendConfig(this) }

    @Test fun defaultIsOffEvenWithLegacyEndpoint() {
        val prefs = mutablePreferencesOf(ip to "10.20.30.40", port to 12345)
        assertEquals(MonitoringSettings(), MonitoringSettings.read(prefs))
        assertEquals("", config(MonitoringSettings.read(prefs)))
    }

    @Test fun enabledEndpointSurvivesReloadAndGeneratesNativeConfig() {
        val prefs = mutablePreferencesOf()
        val settings = MonitoringSettings(true, "192.168.1.12", 12345)
        settings.writeTo(prefs)
        assertEquals(settings, MonitoringSettings.read(prefs))
        assertEquals("[control]\nip=192.168.1.12\nport=12345\nallow=0.0.0.0/0\n\n", config(MonitoringSettings.read(prefs)))
    }

    @Test fun disablingRemovesEndpointAndDoesNotRestoreItOnNextEnable() {
        val prefs = mutablePreferencesOf()
        MonitoringSettings(true, "10.20.30.40", 12345).writeTo(prefs)
        MonitoringSettings(false, "10.20.30.40", 12345).writeTo(prefs)
        assertEquals(false, prefs[enabled]); assertNull(prefs[ip]); assertNull(prefs[port])
        val defaults = MonitoringSettings.read(prefs)
        assertEquals("", config(defaults))
        assertEquals("127.0.0.1", defaults.ip); assertEquals(9999, defaults.port)
        defaults.copy(enabled = true).writeTo(prefs)
        assertEquals(MonitoringSettings(true), MonitoringSettings.read(prefs))
    }

    @Test fun invalidEndpointsCannotPartiallyOverwritePreferences() {
        val prefs = mutablePreferencesOf()
        val good = MonitoringSettings(true)
        good.writeTo(prefs)
        val invalid = listOf("localhost", "::1", "127.1", "127.0.0.256", "01.0.0.1", "127.0.0.1\n[global]")
            .map { MonitoringSettings(true, it) } + listOf(0, -1, 65536).map { MonitoringSettings(true, port = it) }
        for (settings in invalid) {
            assertFalse(settings.valid)
            try { settings.writeTo(prefs); fail("Invalid endpoint saved: $settings") } catch (_: IllegalArgumentException) { }
            assertEquals(good, MonitoringSettings.read(prefs))
        }
        assertTrue(MonitoringSettings(true, port = 1).valid)
        assertTrue(MonitoringSettings(true, port = 65535).valid)
    }
}
