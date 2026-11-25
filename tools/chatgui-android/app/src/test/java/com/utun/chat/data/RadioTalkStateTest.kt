package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class RadioTalkStateTest {
    private fun address(value: Int) = "%02x".format(value).repeat(32)

    private fun event(group: String = address(7), node: Int = 42, on: Boolean = true): RadioTalkEvent {
        val bytes = ByteBuffer.allocate(67).order(ByteOrder.LITTLE_ENDIAN)
            .put(group.hexToByteArray()).put(ByteArray(32) { node.toByte() }).putShort(65535.toShort()).put(if (on) 1.toByte() else 0.toByte()).array()
        return requireNotNull(parseRadioTalkEvent(bytes))
    }

    @Test fun stopRemovesOnlyItsSourceAndMuteDoesNotSuppressSpeech() {
        var state = RadioTalkState(address(7)).onTalk(event()).onTalk(event(node = 43))
        assertEquals(setOf(address(42), address(43)), state.speakers)
        val muted = ChatMember("Alice", radioMuted = true, ed25519 = address(42))
        assertTrue(muted.radioMuted && muted.ed25519 in state.speakers)
        assertSame(state, state.onTalk(event()))
        state = state.onTalk(event(on = false))
        assertEquals(setOf(address(43)), state.speakers)
        assertSame(state, state.onTalk(event(on = false)))
        assertTrue(state.onTalk(event(node = 43, on = false)).speakers.isEmpty())
    }

    @Test fun activeGroupOwnsSpeakersIndependentlyOfDisplayedMemberSnapshots() {
        val group = "fedcba9876543210".repeat(4)
        val state = RadioTalkState(group).onTalk(event(group))
        assertEquals(setOf(address(42)), state.speakers)
        assertSame(state, state.onTalk(event(group = address(8))))
        assertSame(state, state.onTalk(event(group = address(8), on = false)))
        val refreshed = ChatMember("Updated", radioActive = false, radioMuted = true, ed25519 = address(42))
        assertTrue(refreshed.ed25519 in state.speakers)
        val switched = RadioTalkState(address(8))
        assertSame(switched, switched.onTalk(event(group)))
        val stopped = RadioTalkState()
        assertSame(stopped, stopped.onTalk(event(group)))
        assertTrue(RadioTalkState(group).speakers.isEmpty())
    }

    @Test fun malformedEventsCannotChangeTheSpeakerState() {
        assertNull(parseRadioTalkEvent(null)); assertNull(parseRadioTalkEvent(ByteArray(66)))
        assertNull(parseRadioTalkEvent(ByteArray(68))); assertNull(parseRadioTalkEvent(ByteArray(67)))
        val bytes = ByteBuffer.allocate(67).order(ByteOrder.LITTLE_ENDIAN).put(address(7).hexToByteArray()).put(ByteArray(32) { 42 }).putShort(1).put(2).array()
        assertNull(parseRadioTalkEvent(bytes))
        bytes[66] = 1
        assertEquals(1, requireNotNull(parseRadioTalkEvent(bytes)).stream)
        assertEquals(65535, event().stream)
        assertNull(parseRadioTalkEvent(bytes.also { it.fill(0, 0, 32) }))
    }
}
