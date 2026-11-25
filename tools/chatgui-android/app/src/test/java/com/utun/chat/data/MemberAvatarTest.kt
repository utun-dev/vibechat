package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class MemberAvatarTest {
    @Test fun phonePresenceRequiresAnOnlinePeerAndAnExplicitKnownFlag() {
        for ((flags, known, busy) in listOf(Triple(0, false, false), Triple(0x40, true, false),
            Triple(0xC0, true, true), Triple(0x80, false, false))) {
            val bytes = ByteArray(MEMBER_WIRE_SIZE).also { it[it.lastIndex] = 1 }
            bytes[0] = 1; bytes[2] = flags.toByte()
            val online = requireNotNull(parseMemberWire(bytes)).toChatMember()
            assertEquals(known, online.phoneStatusKnown); assertEquals(busy, online.phoneBusy)
            bytes[0] = 0
            val offline = requireNotNull(parseMemberWire(bytes)).toChatMember(online)
            assertFalse(offline.phoneStatusKnown); assertFalse(offline.phoneBusy)
        }
    }
    @Test fun snapshotStringsStopAtFirstNullBeforeDirtyPadding() {
        for (name in listOf("", "0x06B9", "Без имени", "x".repeat(63))) {
            val bytes = ByteArray(MEMBER_WIRE_SIZE).also { it[it.lastIndex] = 1 }
            bytes.fill(0xff.toByte(), 7, 71)
            val encoded = name.toByteArray(Charsets.UTF_8)
            encoded.copyInto(bytes, 7); bytes[7 + encoded.size] = 0
            MemberFilesProtocol.addressBytes("02".repeat(32)).copyInto(bytes, 71)
            bytes.fill(0xff.toByte(), 103, 173); bytes[103] = 0; bytes[173] = 1
            val member = requireNotNull(parseMemberWire(bytes)).toChatMember()
            assertEquals(name, member.name)
            assertEquals("02".repeat(32), member.avatarServer); assertEquals("", member.avatarFile); assertTrue(member.storageRole)
        }
    }

    @Test fun sharedSnapshotKeepsAvatarAndSignedStorageRole() {
        val bytes = ByteArray(MEMBER_WIRE_SIZE).also { it[it.lastIndex] = 1 }
        val b = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        bytes[0] = 1; bytes[2] = 2
        "Alice".toByteArray().copyInto(bytes, 7)
        val server = "fedcba9876543210".repeat(4)
        MemberFilesProtocol.addressBytes(server).copyInto(bytes, 71)
        val file = "b".repeat(64) + ".json"
        file.toByteArray().copyInto(bytes, 103); bytes[173] = 1
        val previous = ChatMember("Old", isVerified = true, nodeType = 3, directUp = true, bgpNodes = 7)
        bytes[174] = 1; bytes[175] = 3; bytes[176] = 5; bytes[177] = 5
        b.putShort(178, 7)
        val member = requireNotNull(parseMemberWire(bytes)).toChatMember(previous)
        assertEquals("Alice", member.name); assertTrue(member.online); assertTrue(member.isAdmin)
        assertEquals(server, member.avatarServer); assertEquals(file, member.avatarFile); assertTrue(member.storageRole)
        assertTrue(member.isVerified); assertEquals(3, member.nodeType); assertTrue(member.directUp); assertEquals(7, member.bgpNodes)
        // Сеть передаёт storage priority отдельно от подписанной роли хранителя.
        assertFalse(member.isStorage)
    }
    @Test fun clearingReferenceDoesNotKeepAnOldAvatar() {
        val previous = ChatMember("Alice", avatarServer = "02".repeat(32), avatarFile = "b".repeat(64) + ".json", storageRole = true)
        val updated = MemberWire(1, true, 0, 0, 0, false, "Alice").toChatMember(previous)
        assertEquals("", updated.avatarServer); assertEquals("", updated.avatarFile); assertFalse(updated.storageRole)
        assertTrue(updated.isSelf)
    }
    @Test fun recordsAtAnOffsetRequireTheCompleteSnapshot() {
        val bytes = ByteArray(MEMBER_WIRE_SIZE + 3)
        bytes[bytes.lastIndex] = 7
        assertEquals("00".repeat(31) + "07", parseMemberWire(bytes, 3)?.ed25519)
        assertNull(parseMemberWire(bytes, 4)); assertNull(parseMemberWire(bytes, -1))
        assertNull(parseMemberWire(bytes.copyOf(MEMBER_WIRE_SIZE - 1)))
    }
}
