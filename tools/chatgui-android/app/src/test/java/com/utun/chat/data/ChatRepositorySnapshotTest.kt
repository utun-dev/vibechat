package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

/** Повреждение второй строки не должно публиковать первый элемент как полный снимок. */
class ChatRepositorySnapshotTest {
    private val errors = mutableListOf<String>()
    private val repo = ChatRepository { errors.add(it) }

    @Test fun conversationParseFailureDiffersFromAValidEmptySnapshotAndAllowsRetry() {
        val row = """{"conv_id":"123","peer_node_id":4,"peer_name":"Узел 😀","generation":2}"""
        assertNull(repo.parseDmConversations("[$row,{}]"))
        assertTrue(errors.single().contains("PM conversation snapshot failed"))
        assertEquals(emptyList<DmConversation>(), repo.parseDmConversations("[]"))
        val restored = repo.parseDmConversations("[$row]")!!.single()
        assertEquals("Узел 😀", restored.peerName); assertEquals(2L, restored.generation)
    }

    @Test fun invalidInviteCannotPublishAPartialMessageHistory() {
        val row = """{"seq":1,"author":"4","data":"Текст 😀","ts":50}"""
        assertNull(repo.parseDmMessages("[$row,{\"ct\":\"invite\",\"tags\":{}}]"))
        assertTrue(errors.single().contains("PM message snapshot failed"))
        assertEquals(emptyList<Message>(), repo.parseDmMessages("[]"))
        assertEquals("Текст 😀", repo.parseDmMessages("[$row]")!!.single().text)
    }

    @Test fun missingRequiredNatFieldsDoNotClearOrTruncateTheSocketSnapshot() {
        val row = """{"si":1,"nm":"as_eth_v4_udp","nmapping":1,"ninbound":1,"nsource":1,"external":"","externalPort":0}"""
        val before = repo.parseLocalSockets("[$row]")!!
        assertNull(repo.parseLocalSockets("[$row,{}]"))
        assertTrue(errors.single().contains("Socket snapshot parse failed"))
        assertEquals(emptyList<LocalSocketState>(), repo.parseLocalSockets("[]"))
        assertEquals(before, repo.parseLocalSockets("[$row]"))
    }

    @Test fun brokenLinkSnapshotDoesNotTurnAConnectedPeerIntoDisconnected() {
        val row = """{"cn":true,"in":true,"lu":1,"lk":[{"id":9,"di":1,"ip":true,"st":3,"sn":"Wi-Fi 😀"}]}"""
        val connected = repo.parseMemberLinks(row)!!
        assertTrue(connected.connected)
        assertEquals("Wi-Fi 😀", connected.links.single().socketName)
        assertNull(repo.parseMemberLinks("""{"cn":true,"lk":[{},null]}"""))
        assertTrue(errors.single().contains("Member links snapshot failed"))
        assertEquals(MemberLinks(), repo.parseMemberLinks("{}"))
        assertEquals(MemberLinks(), repo.parseMemberLinks("""{"cn":false}"""))
        assertEquals(connected, repo.parseMemberLinks(row))
    }
    @Test fun bleSnapshotPreservesDirectionAndSeparatesGattFromEtcpReadiness() {
        for ((wire, direction) in LinkDirection.entries.withIndex()) {
            val row = """{"cn":true,"lk":[{"pt":"BLE","di":$wire,"ip":false,"st":1,"ls":0}]}"""
            val link = repo.parseMemberLinks(row)!!.links.single()
            assertEquals("BLE", link.protocol); assertEquals(direction, link.direction)
            assertFalse(link.hasIp); assertEquals(1, link.linkState); assertEquals(0, link.linkStatus)
            assertEquals("", link.localAddr); assertEquals("", link.remoteAddr)
        }
        assertNull(repo.parseMemberLinks("""{"cn":true,"lk":[{"di":3,"ip":false}]}"""))
        assertTrue(errors.single().contains("Invalid link direction"))
    }

    @Test fun mixedBleAndIpLinksKeepTheirOwnTransportAndEndpoints() {
        val json = """{"cn":true,"lk":[
            {"pt":"BLE","di":2,"ip":false,"st":3,"ls":1},
            {"pt":"TCP+REALITY","di":1,"ip":true,"la":"::1","lp":1234,"ra":"::2","rp":443,"st":3,"ls":1}
        ]}"""
        val links = repo.parseMemberLinks(json)!!.links
        assertEquals(2, links.size); assertFalse(links[0].hasIp); assertTrue(links[1].hasIp)
        assertEquals("TCP+REALITY", links[1].protocol); assertEquals("::2", links[1].remoteAddr); assertEquals(443, links[1].remotePort)
    }

}
