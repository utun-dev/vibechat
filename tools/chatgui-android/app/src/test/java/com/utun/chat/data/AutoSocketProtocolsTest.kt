package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class AutoSocketProtocolsTest {
    @Test fun createsOnlySelectedProtocolsForBothFamilies() {
        for (mode in AutoSocketProtocols.values()) {
            for (ip in listOf("192.168.1.2", "fd42::2")) {
                val entries = mode.createServers("wlan_0", ip, emptyList())
                assertEquals(mode.protocols, entries.map { it.protocol })
                assertTrue(entries.all { it.ifName == "wlan_0" && it.bindIp == ip && it.port in 2000..9999 })
            }
        }
    }

    @Test fun keepsPortsSeparateByInterfaceFamilyAndProtocol() {
        val saved = listOf(
            ServerEntry("1", "wlan0", "192.168.1.2", "tcp", 2001),
            ServerEntry("2", "wlan0", "192.168.1.2", "udp", 2002),
            ServerEntry("3", "wlan0", "fd42::2", "tcp", 2003),
            ServerEntry("4", "wlan0", "fd42::2", "udp", 2004),
            ServerEntry("5", "rmnet0", "192.168.1.2", "tcp", 2005)
        )
        val v4 = AutoSocketProtocols.BOTH.createServers("wlan0", "192.168.2.3", saved)
        val v6 = AutoSocketProtocols.BOTH.createServers("wlan0", "fd42::3", saved)
        assertEquals(listOf(2001, 2002), v4.map { it.port })
        assertEquals(listOf(2003, 2004), v6.map { it.port })
        assertEquals(2005, AutoSocketProtocols.TCP.createServers("rmnet0", "10.0.0.2", saved).single().port)
        assertEquals(4, (v4 + v6).map { it.sectionName }.toSet().size)
        assertEquals(4, (v4 + v6).map { it.socketKey }.toSet().size)
    }

    @Test fun restoresDisabledProtocolsAndMissingInterfaces() {
        val saved = AutoSocketProtocols.BOTH.createServers("wlan0", "192.168.1.2", emptyList()) +
            AutoSocketProtocols.BOTH.createServers("rmnet0", "10.0.0.2", emptyList())
        val active = AutoSocketProtocols.TCP.createServers("wlan0", "192.168.2.3", saved)
        val activeKeys = active.map { it.socketKey }.toSet()
        val inactive = saved.filter { it.socketKey !in activeKeys }
        assertEquals(3, inactive.size)
        val restored = AutoSocketProtocols.BOTH.createServers("wlan0", "192.168.3.4", active + inactive) +
            AutoSocketProtocols.BOTH.createServers("rmnet0", "10.0.1.2", active + inactive)
        assertEquals(saved.associate { it.socketKey to it.port }, restored.associate { it.socketKey to it.port })
    }
}
