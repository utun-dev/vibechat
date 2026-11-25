package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class UdpLogSettingsTest {
    private fun config(settings: UdpLogSettings): String = buildString { appendLine("[global]"); settings.appendConfig(this) }

    @Test fun defaultAddressDoesNotEnableNetworkLogging() {
        val settings = UdpLogSettings()
        assertFalse(settings.enabled)
        assertFalse(settings.active)
        assertEquals("[global]\n", config(settings))
    }

    @Test fun explicitEnableWritesTargetIntoStartupConfig() {
        val settings = UdpLogSettings(true, "192.168.40.250", 9999)
        assertTrue(settings.active)
        assertEquals("[global]\n\n[log_udp]\nip=192.168.40.250\nport=9999\n", config(settings))
    }

    @Test fun disabledTargetRetainsAddressWithoutConfiguringSender() {
        val enabled = UdpLogSettings(true, "10.20.30.40", 12345)
        val disabled = enabled.copy(enabled = false)
        assertEquals(enabled.ip, disabled.ip)
        assertEquals(enabled.port, disabled.port)
        assertEquals("[global]\n", config(disabled))
        assertEquals(config(enabled), config(disabled.copy(enabled = true)))
    }

    @Test fun malformedTargetsNeverEnableSender() {
        for (ip in listOf("", "localhost", "127.1", "1.2.3.256", "01.2.3.4", "1.2.3.4 ")) {
            val settings = UdpLogSettings(true, ip, 9999)
            assertFalse(ip, settings.active)
            assertEquals("[global]\n", config(settings))
        }
        for (port in listOf(-1, 0, 65536)) assertFalse(UdpLogSettings(true, "127.0.0.1", port).active)
        assertTrue(UdpLogSettings(true, "127.0.0.1", 65535).active)
    }
}
