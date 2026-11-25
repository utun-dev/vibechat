package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class DmMembersTest {
    private val target = ConversationTarget.Pm("42", 7)
    private fun frame(vararg entries: Triple<Int, Long, Boolean>): ByteArray {
        val data = ByteArray(13 + entries.size * (MEMBER_WIRE_SIZE + 1))
        data[0] = 2; data[1] = '4'.code.toByte(); data[2] = '2'.code.toByte()
        ByteBuffer.wrap(data, 3, 10).order(ByteOrder.LITTLE_ENDIAN).putLong(7).putShort(entries.size.toShort())
        entries.forEachIndexed { i, (side, node, self) ->
            val offset = 13 + i * (MEMBER_WIRE_SIZE + 1); data[offset] = side.toByte()
            data[offset + 1] = if (self) 1 else 0; data[offset + 2] = if (self) 1 else 0
            "node$node".toByteArray().copyInto(data, offset + 8)
            data[offset + MEMBER_WIRE_SIZE] = node.toByte()
        }
        return data
    }

    @Test fun separatesOwnAndPeerDevicesIncludingOffline() {
        val members = parseDmMembers(frame(Triple(1, 9L, false), Triple(0, 7L, false), Triple(0, 21L, true)), target)
        assertEquals(listOf("15", "07", "09"), members.map { it.member.ed25519.takeLast(2) })
        assertEquals(listOf(0, 0, 1), members.map { it.side })
        assertEquals(listOf(true, false, false), members.map { it.member.online })
        assertTrue(members.first().member.isSelf)
    }

    @Test fun emptyOrFallbackSideDoesNotRequireAGroup() {
        assertTrue(parseDmMembers(frame(), target).isEmpty())
        val members = parseDmMembers(frame(Triple(1, 9L, false)), target)
        assertEquals(1, members.size); assertEquals("", members.single().member.devgroup)
    }

    @Test fun keepsDevicesWithTheSameNameAndDistinctAddresses() {
        val data = frame(Triple(0, 7L, false), Triple(1, 7L, false))
        data[data.lastIndex] = 9
        val members = parseDmMembers(data, target)
        assertEquals(listOf("07", "09"), members.map { it.member.ed25519.takeLast(2) })
    }

    @Test fun rejectsOtherConversationGenerationAndMalformedRecords() {
        val data = frame(Triple(0, 7L, true), Triple(1, 9L, false))
        fun rejected(bytes: ByteArray, selected: ConversationTarget.Pm = target) {
            assertThrows(IllegalArgumentException::class.java) { parseDmMembers(bytes, selected) }
        }
        rejected(data, target.copy(id = "43")); rejected(data, target.copy(generation = 8))
        rejected(data.copyOf(data.size - 1)); rejected(data + 0.toByte())
        rejected(frame(Triple(0, 7L, true), Triple(1, 7L, false)))
        rejected(frame(Triple(2, 9L, false))); rejected(frame(Triple(1, 0L, false)))
    }
}
