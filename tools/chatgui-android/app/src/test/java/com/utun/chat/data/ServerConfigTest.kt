package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

/** Сохранённые порты проверяются до повторного использования в автосокетах; ошибки сохраняют остальные секции. */
class ServerConfigTest {
    private fun section(name: String, port: String, transport: String = "tcp") =
        "[server:$name]\naddr=0.0.0.0:$port\ntransport=$transport\n"

    @Test fun roundTripIdentityUsesInterfaceFamilyAndTransport() {
        val config = section("wlan_0_v4_tcp", "2001") + section("wlan_0_v4_udp", "2002", "udp") +
            "[server:wlan_0_v6_tcp]\naddr=[::]:2003\ntransport=tcp\n"
        val servers = ServerConfig.parse(config.lineSequence(), false) { fail(it) }
        assertEquals(listOf(2001, 2002, 2003), servers.map { it.port })
        assertEquals(listOf("wlan_0_v4_tcp", "wlan_0_v4_udp", "wlan_0_v6_tcp"), servers.map { it.sectionName })
        assertEquals(listOf("0.0.0.0", "0.0.0.0", "::"), servers.map { it.bindIp })
        assertEquals(listOf(2001, 2002), AutoSocketProtocols.BOTH.createServers("wlan_0", "192.0.2.1", servers).map { it.port })
    }

    @Test fun invalidSavedPortsAreSkippedBeforeAutoReuse() {
        for (port in listOf("0", "-1", "65536", "2147483648", "invalid", "")) {
            val warnings = mutableListOf<String>()
            val config = section("wlan0_v4_tcp", port) + section("other_v4_tcp", "3456")
            val servers = ServerConfig.parse(config.lineSequence(), false, warnings::add)
            assertEquals(listOf("other"), servers.map { it.ifName })
            assertEquals(listOf(3456), servers.map { it.port })
            assertEquals(1, warnings.size)
            assertTrue(warnings.single(), warnings.single().contains("wlan0_v4_tcp"))
            val recovered = AutoSocketProtocols.TCP.createServers("wlan0", "192.0.2.1", servers).single()
            assertTrue(recovered.port in 2000..9999)
        }
    }

    @Test fun bothPortBoundariesArePreserved() {
        val config = section("first_v4_tcp", "1") + section("last_v4_udp", "65535", "udp")
        assertEquals(listOf(1, 65535), ServerConfig.parse(config.lineSequence(), false) { fail(it) }.map { it.port })
    }

    @Test fun commentedSectionsAreOnlyRestoredWhenRequested() {
        val inactive = section("offline_v6_udp", "4567", "udp").lineSequence().filter { it.isNotEmpty() }.joinToString("\n") { "#$it" }
        val config = section("live_v4_tcp", "2345") + inactive
        val active = ServerConfig.parse(config.lineSequence(), false) { fail(it) }
        val all = ServerConfig.parse(config.lineSequence(), true) { fail(it) }
        assertEquals(listOf("live"), active.map { it.ifName })
        assertEquals(listOf("live", "offline"), all.map { it.ifName })
        assertEquals("::", all.last().bindIp)
        assertEquals("udp", all.last().protocol)
    }

    @Test fun commentedFieldsDoNotOverrideActiveFields() {
        val config = section("wlan0_v4_tcp", "2345") + "#addr=0.0.0.0:9999\n#transport=udp\n"
        for (includeCommented in listOf(false, true)) {
            val server = ServerConfig.parse(config.lineSequence(), includeCommented) { fail(it) }.single()
            assertEquals(2345, server.port)
            assertEquals("tcp", server.protocol)
        }
    }

    @Test fun unknownSectionsDoNotLeakFieldsIntoServer() {
        val config = "[global]\naddr=0.0.0.0:9999\n" + section("wlan0_v4_tcp", "2345") + "[debug]\ntransport=udp\n"
        val server = ServerConfig.parse(config.lineSequence(), false) { fail(it) }.single()
        assertEquals(2345, server.port)
        assertEquals("tcp", server.protocol)
    }

    @Test fun invalidSectionAndTransportAreDiagnosed() {
        val warnings = mutableListOf<String>()
        val config = section("wlan0_unknown_tcp", "2345") + section("wlan0_v4_tcp", "2345", "unknown") +
            "[server:missing_v4_tcp]\ntransport=tcp\n"
        assertTrue(ServerConfig.parse(config.lineSequence(), false, warnings::add).isEmpty())
        assertEquals(3, warnings.size)
    }

    @Test fun transportFieldWinsAndSuffixSuppliesDefault() {
        val config = section("wlan0_v4_tcp", "2345", "udp") + "[server:wlan0_v6_udp]\naddr=[::]:3456\n" +
            "[server:old_v4]\naddr=0.0.0.0:4567\n"
        val servers = ServerConfig.parse(config.lineSequence(), false) { fail(it) }
        assertEquals(listOf("udp", "udp", "tcp"), servers.map { it.protocol })
    }
}
