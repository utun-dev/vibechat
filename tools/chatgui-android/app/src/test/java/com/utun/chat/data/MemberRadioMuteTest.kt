package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class MemberRadioMuteTest {
    @Test fun muteUpdatesKeepRadioSubscriptionAndOtherProperties() {
        val bytes = ByteArray(MEMBER_WIRE_SIZE)
        bytes[bytes.lastIndex] = 42
        bytes[0] = 1; bytes[2] = 0x32
        bytes[174] = 1; bytes[175] = 4; bytes[176] = 1; bytes[177] = 1
        val previous = ChatMember("Alice", isVerified = true, directUp = true, nodeType = 4)
        val muted = requireNotNull(parseMemberWire(bytes)).toChatMember(previous)
        assertTrue(muted.radioMuted); assertTrue(muted.radioActive); assertTrue(muted.isAdmin)
        assertTrue(muted.isVerified); assertTrue(muted.directUp); assertEquals(4, muted.nodeType)
        bytes[2] = 0x12
        val listening = requireNotNull(parseMemberWire(bytes)).toChatMember(muted)
        assertFalse(listening.radioMuted); assertTrue(listening.radioActive); assertTrue(listening.isVerified)
        bytes[2] = 0x20
        val offlineRadio = requireNotNull(parseMemberWire(bytes)).toChatMember(listening)
        assertTrue(offlineRadio.radioMuted); assertFalse(offlineRadio.radioActive)
    }
}
