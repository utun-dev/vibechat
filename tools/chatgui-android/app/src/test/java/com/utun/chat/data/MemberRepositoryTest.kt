package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class MemberRepositoryTest {
    @Test fun deviceGroupAvatarFindsFirstPublishedPhotoRegardlessOfOnline() {
        val first = ChatMember("First", online = true)
        val second = ChatMember("Second", online = false, avatarServer = "63".repeat(32), avatarFile = "photo.json")
        val third = second.copy(ed25519 = key(9), online = true)
        assertEquals(second, deviceGroupAvatar(listOf(first, second, third)))
        assertEquals(third, deviceGroupAvatar(listOf(first, second.copy(isDeleted = true), third)))
        assertEquals(first, deviceGroupAvatar(listOf(first, second.copy(avatarFile = ""))))
        assertEquals(first, deviceGroupAvatar(listOf(first, second.copy(avatarServer = ""))))
        assertEquals(first.copy(avatarServer = "63".repeat(32), avatarFile = "first.json"),
            deviceGroupAvatar(listOf(first.copy(avatarServer = "63".repeat(32), avatarFile = "first.json"), second)))
        assertNull(deviceGroupAvatar(listOf(second.copy(isDeleted = true))))
    }

    @Test fun protocolWarningSurvivesOfflineSnapshotAndClearsOnCompatibleUpdate() {
        val bytes = ByteArray(MEMBER_WIRE_SIZE)
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putShort(277, 1).putShort(279, 2)
        bytes[MEMBER_WIRE_SIZE - 1] = 7
        bytes[276] = 2
        val incompatible = requireNotNull(parseMemberWire(bytes)).toChatMember()
        assertFalse(incompatible.online)
        assertEquals(2, incompatible.protocolState)
        assertEquals(1, incompatible.protocolLocal)
        assertEquals(2, incompatible.protocolRemote)
        bytes[276] = 1; buffer.putShort(279, 1)
        val compatible = requireNotNull(parseMemberWire(bytes)).toChatMember(incompatible)
        assertEquals(1, compatible.protocolState)
        assertEquals(1, compatible.protocolRemote)
        assertNull(parseMemberWire(bytes.copyOf(MEMBER_WIRE_SIZE - 1)))
    }

    @Test fun linkingOnlyAcceptsLiveBgpPeersOutsideDeviceGroups() {
        val peer = ChatMember(name = "Peer", online = true, bgpUp = true, ed25519 = "07".repeat(32))
        assertTrue(peer.canLinkAsMyDevice)
        for (ineligible in listOf(peer.copy(ed25519 = ""), peer.copy(isSelf = true), peer.copy(isDeleted = true), peer.copy(online = false),
            peer.copy(bgpUp = false, directUp = true), peer.copy(bgpUp = false, bgpPresence = true),
            peer.copy(devgroup = "2a" + "00".repeat(31), devgroupName = ""))) assertFalse(ineligible.canLinkAsMyDevice)
    }
    @org.junit.Test fun deviceGroupSurvivesTheSharedMemberSnapshot() {
        val bytes = ByteArray(MEMBER_WIRE_SIZE)
        bytes[MEMBER_WIRE_SIZE - 1] = 7
        bytes[180] = 42
        "Мои устройства".toByteArray(Charsets.UTF_8).copyInto(bytes, 212)
        val member = requireNotNull(parseMemberWire(bytes)).toChatMember()
        org.junit.Assert.assertEquals("2a" + "00".repeat(31), member.devgroup)
        org.junit.Assert.assertEquals("Мои устройства", member.devgroupName)
        org.junit.Assert.assertEquals("00".repeat(31) + "07", member.ed25519)
    }
    private fun key(node: Int): String = "%02x".format(node).repeat(32)

    private fun row(node: Long, flags: Int = 0x40, online: Boolean = true, up: Int = 5, rtt: Int = 30, key: Byte = node.toByte()): ByteArray =
        ByteArray(MEMBER_WIRE_SIZE).also { bytes ->
            ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putShort(3, rtt.toShort())
            bytes[0] = if (online) 1 else 0; bytes[2] = flags.toByte()
            "Member".toByteArray().copyInto(bytes, 7)
            bytes[174] = 1; bytes[175] = 4; bytes[176] = 5; bytes[177] = up.toByte()
            bytes.fill(key, MEMBER_WIRE_SIZE - 32)
        }

    private fun event(group: String, vararg rows: ByteArray, full: Boolean = true): ByteArray {
        val offset = 1 + group.length + if (full) 2 else 1
        return ByteArray(offset + rows.size * MEMBER_WIRE_SIZE).also { bytes ->
            bytes[0] = group.length.toByte(); group.toByteArray().copyInto(bytes, 1)
            bytes[1 + group.length] = rows.size.toByte()
            rows.forEachIndexed { index, row -> row.copyInto(bytes, offset + index * MEMBER_WIRE_SIZE) }
        }
    }

    private fun repository(request: (String) -> Boolean = { true }, now: () -> Long = { 0 }): MemberRepository =
        MemberRepository(request, { true }, now, { _, _ -> }).also { it.onEvent(12, null) }

    @Test fun deletedAndAbsentMembersDoNotStartARequestLoop() {
        var requests = 0
        val repo = repository({ requests++; true })
        repeat(100) { repo.ensure(key(0x12)) }
        assertEquals(1, requests)
        repo.onEvent(23, event(key(0x12), row(1), row(2, flags = 8, online = false, up = 0)))
        repeat(100) { repo.ensure(key(0x12)) }
        assertEquals(1, requests); assertTrue(repo.member(key(0x12), key(2))!!.isDeleted)
        assertNull(repo.member(key(0x12), key(3)))
        repo.refresh(key(0x12)); assertEquals(2, requests)
        repo.onEvent(23, event(key(0x12)))
        repeat(100) { repo.ensure(key(0x12)) }
        assertEquals(2, requests); assertTrue(repo.list(key(0x12)).isEmpty())
    }

    @Test fun phoneRouteAndRttRemainConsistentAcrossRefreshAndDifferentGroups() {
        val repo = repository()
        repo.onEvent(23, event(key(0x12), row(1)))
        val before = repo.member(key(0x12), key(1))!!
        repo.refresh(key(0x12))
        assertEquals(before, repo.member(key(0x12), key(1)))
        repo.onEvent(23, event(key(0x12), row(1)))
        assertEquals(before, repo.member(key(0x12), key(1)))
        repo.onEvent(23, event(key(0x45), row(1, online = false, up = 0, rtt = 0xffff)))
        assertEquals(before, repo.member(key(0x12), key(1))); assertFalse(repo.member(key(0x45), key(1))!!.online)
        repo.onEvent(24, event(key(0x12), row(1, flags = 0xc0, up = 4, rtt = 80), full = false))
        val relay = repo.member(key(0x12), key(1))!!
        assertTrue(relay.online); assertTrue(relay.phoneBusy); assertFalse(relay.directUp)
        assertTrue(relay.bgpUp); assertEquals(80, relay.rtt); assertTrue(relay.isVerified)
        val detail = MemberDetail(key(1), online = false, channelId = key(0x12)).withMember(relay)
        assertTrue(detail.online); assertFalse(detail.directUp); assertEquals(relay.rtt, detail.bestRtt)
        repo.onEvent(24, event(key(0x12), row(1, flags = 0x40), full = false))
        val free = repo.member(key(0x12), key(1))!!
        assertTrue(free.phoneStatusKnown); assertFalse(free.phoneBusy)
        repo.onEvent(24, event(key(0x12), row(1, flags = 0), full = false))
        val unknown = repo.member(key(0x12), key(1))!!
        assertFalse(unknown.phoneStatusKnown); assertFalse(unknown.phoneBusy)
    }

    @Test fun malformedSnapshotDoesNotPartiallyReplaceGoodState() {
        val repo = repository()
        repo.onEvent(23, event(key(0x12), row(1)))
        val before = repo.members.value
        repo.onEvent(23, event(key(0x12), row(2)).dropLast(1).toByteArray())
        assertEquals(before, repo.members.value)
        repo.onEvent(23, event(key(0x12), row(2), row(2)))
        assertEquals(before, repo.members.value)
    }

    @Test fun distinctKeysWithTheSameNumericProjectionKeepSeparateRows() {
        val repo = repository()
        repo.onEvent(23, event(key(0x12), row(1), row(1, key = 2)))
        assertEquals(2, repo.list(key(0x12)).size)
        val removed = byteArrayOf(64) + key(0x12).toByteArray() + ByteArray(32) { 2 }
        repo.onEvent(25, removed)
        assertNotNull(repo.member(key(0x12), key(1)))
        assertNull(repo.member(key(0x12), key(2)))
    }

    @Test fun stopDiscardsLiveStateAndLateEventsAndAllowsFreshStart() {
        val repo = repository()
        repo.onEvent(23, event(key(0x12), row(1)))
        val generation = repo.generation
        repo.onEvent(13, null)
        repo.onEvent(23, event(key(0x12), row(1)))
        assertTrue(repo.members.value.isEmpty()); assertTrue(repo.generation > generation)
        repo.onEvent(12, null); repo.onEvent(23, event(key(0x12), row(1, flags = 0)))
        assertFalse(repo.member(key(0x12), key(1))!!.phoneStatusKnown)
    }

    @Test fun failedAndTimedOutRequestsCanRetryWithoutClearingLoadedState() {
        var now = 0L; var requests = 0
        val repo = repository({ ++requests != 1 }, { now })
        repo.ensure(key(0x12)); repo.ensure(key(0x12)); assertEquals(2, requests)
        now = 10_001; repo.ensure(key(0x12)); assertEquals(3, requests)
        repo.onEvent(23, event(key(0x12), row(1)))
        repo.refresh(key(0x12)); assertTrue(repo.member(key(0x12), key(1))!!.phoneStatusKnown)
    }

    @Test fun enqueueExceptionReleasesPendingAndKeepsLastSnapshot() {
        var requests = 0
        val diagnostics = mutableListOf<String>()
        val repo = MemberRepository({ if (++requests == 1) error("JNI request failed") else true }, { true }, { 0 },
            { _, message -> diagnostics.add(message) })
        repo.onEvent(12, null); repo.onEvent(23, event(key(0x12), row(1)))
        val before = repo.members.value
        repo.refresh(key(0x12)); repo.refresh(key(0x12))
        assertEquals(2, requests); assertEquals(before, repo.members.value)
        assertTrue(diagnostics.any { "enqueue threw" in it })
    }

    @Test fun malformedDeletionCannotClearAnOtherwiseValidSnapshot() {
        val repo = repository(); repo.onEvent(23, event(key(0x12), row(1)))
        val before = repo.members.value
        val header = byteArrayOf(64) + key(0x12).toByteArray()
        repo.onEvent(31, header + byteArrayOf(0))
        assertEquals(before, repo.members.value)
        repo.onEvent(31, header)
        assertTrue(repo.members.value.isEmpty())
    }
}
