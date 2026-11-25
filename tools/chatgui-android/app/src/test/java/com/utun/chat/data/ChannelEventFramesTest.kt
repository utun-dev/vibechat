package com.utun.chat.data

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.*
import org.junit.Test

class ChannelEventFramesTest {
    private fun frame(tail: Int, fill: ByteBuffer.() -> Unit): ByteArray =
        ByteBuffer.allocate(3 + tail).order(ByteOrder.LITTLE_ENDIAN).put(2).put("42".toByteArray()).apply(fill).array()
    private fun rejected(data: ByteArray?, parse: (ByteArray?) -> Unit) {
        try { parse(data); fail("Malformed frame accepted") } catch (_: IllegalArgumentException) { }
    }
    @Test fun connectingRequiresCompleteCountAndPreservesKeys() {
        val bytes = frame(66) { putShort(2); put(ByteArray(32) { 7 }); put(ByteArray(32) { 0xff.toByte() }) }
        assertEquals(ChannelEventFrames.Nodes("42", listOf("07".repeat(32), "ff".repeat(32))), ChannelEventFrames.nodes(bytes))
        for (end in bytes.indices) rejected(bytes.copyOf(end)) { ChannelEventFrames.nodes(it) }
        rejected(bytes + 0) { ChannelEventFrames.nodes(it) }
        rejected(null) { ChannelEventFrames.nodes(it) }
    }
    @Test fun candidatesValidateWholeListBeforeReturningAutomaticNode() {
        val auto = ByteArray(32) { 7 }; val other = ByteArray(32) { 9 }
        val bytes = frame(66) { put(auto); putShort(1); put(other) }
        assertEquals(ChannelEventFrames.Candidates("42", "07".repeat(32), listOf("09".repeat(32))), ChannelEventFrames.candidates(bytes))
        for (end in bytes.indices) rejected(bytes.copyOf(end)) { ChannelEventFrames.candidates(it) }
        rejected(bytes + 0) { ChannelEventFrames.candidates(it) }
        assertTrue(ChannelEventFrames.candidates(frame(34) { put(ByteArray(32)); putShort(0) }).addresses.isEmpty())
    }
    @Test fun inviteDistinguishesExplicitFailureFromTruncatedSuccess() {
        val text = "utun://😀".toByteArray()
        val bytes = frame(10 + text.size) { putLong(991); putShort(text.size.toShort()); put(text) }
        assertEquals(ChannelEventFrames.Invite("42", 991, "utun://😀"), ChannelEventFrames.invite(bytes))
        for (end in bytes.indices) rejected(bytes.copyOf(end)) { ChannelEventFrames.invite(it) }
        rejected(bytes + 0) { ChannelEventFrames.invite(it) }
        assertEquals(ChannelEventFrames.Invite("42", 991, null), ChannelEventFrames.invite(frame(10) { putLong(991); putShort(0) }))
    }
}
