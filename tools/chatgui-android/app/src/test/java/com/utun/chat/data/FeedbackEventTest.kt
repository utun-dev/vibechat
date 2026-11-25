package com.utun.chat.data

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CharacterCodingException
import org.junit.Assert.*
import org.junit.Test

/** Настоящий wire-разбор, используемый ViewModel; формат фиксируют chat_event.h и C-производители событий. */
class FeedbackEventTest {
    private val group = "ab".repeat(32)

    private fun changed(channel: String = group, kind: Int = 1): ByteArray {
        val text = channel.toByteArray(Charsets.UTF_8)
        return byteArrayOf(text.size.toByte()) + text + ByteArray(32) { it.toByte() } + byteArrayOf(kind.toByte())
    }
    private fun result(id: Long = 7, flag: Int = 1, text: ByteArray = "{}".toByteArray()): ByteArray =
        ByteBuffer.allocate(9 + text.size).order(ByteOrder.LITTLE_ENDIAN).putLong(id).put(flag.toByte()).put(text).array()

    @Test fun changedEventKeepsCanonicalChannelMessageAndFeedbackKind() {
        val event = requireNotNull(parseFeedbackChangedEvent(changed(group, 2)))
        assertEquals(group, event.channelId); assertEquals(2, event.kind)
        assertEquals("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", event.messageId)
    }

    @Test fun truncatedChangedEventsFailInsteadOfBeingSilentlyIgnored() {
        assertThrows(IllegalArgumentException::class.java) { parseFeedbackChangedEvent(null) }
        for (size in 0..34) assertThrows(IllegalArgumentException::class.java) { parseFeedbackChangedEvent(ByteArray(size)) }
        val valid = changed()
        assertThrows(IllegalArgumentException::class.java) { parseFeedbackChangedEvent(valid.copyOf(valid.size - 1)) }
        assertThrows(IllegalArgumentException::class.java) { parseFeedbackChangedEvent(valid + byteArrayOf(0)) }
    }

    @Test fun invalidChannelAndUnknownKindCannotTriggerASnapshotRequest() {
        for (channel in listOf("", "0", "01", "-1", "+1", "1a", "é", "9223372036854775808", "1".repeat(20), "AB".repeat(32), "0".repeat(64))) {
            assertThrows(IllegalArgumentException::class.java) { parseFeedbackChangedEvent(changed(channel)) }
        }
        for (kind in listOf(0, 3, 255)) {
            assertThrows(IllegalArgumentException::class.java) { parseFeedbackChangedEvent(changed(kind = kind)) }
        }
    }

    @Test fun resultPreservesAllRequestBitsAndUnicodeErrorText() {
        val id = 0xfedcba9876543210uL.toLong()
        val event = requireNotNull(parseFeedbackResultEvent(result(id, 0, "{\"error\":\"Отказ 😀\"}".toByteArray())))
        assertEquals(id, event.requestId); assertFalse(event.success); assertEquals("Отказ 😀", event.json.getString("error"))
        val background = requireNotNull(parseFeedbackResultEvent(result(0)))
        assertEquals(0L, background.requestId); assertTrue(background.success)
    }

    @Test fun truncatedResultAndEmptyJsonFailForTheCallerToReport() {
        assertThrows(IllegalArgumentException::class.java) { parseFeedbackResultEvent(null) }
        for (size in 0..9) assertThrows(IllegalArgumentException::class.java) { parseFeedbackResultEvent(ByteArray(size)) }
    }

    @Test fun successFlagMustBeExactlyZeroOrOne() {
        for (flag in listOf(2, 127, 255)) {
            assertThrows(IllegalArgumentException::class.java) { parseFeedbackResultEvent(result(flag = flag)) }
        }
    }

    @Test fun invalidUtf8AndMalformedJsonAreNotReplacedByAValidSnapshot() {
        val corrupt = "{\"text\":\"".toByteArray() + byteArrayOf(0xff.toByte()) + "\"}".toByteArray()
        assertThrows(CharacterCodingException::class.java) { parseFeedbackResultEvent(result(text = corrupt)) }
        for (json in listOf("{broken}", "[]", "{} {}", "{} garbage", "{}\u0000garbage")) {
            assertThrows(Exception::class.java) { parseFeedbackResultEvent(result(text = json.toByteArray())) }
        }
    }

    @Test fun committedAckAndCatalogRemainDifferentFromASnapshot() {
        val ack = "{\"ch\":\"$group\",\"message_id\":\"${"ab".repeat(32)}\",\"committed\":true,\"snapshot_pending\":true}"
        val event = requireNotNull(parseFeedbackResultEvent(result(text = ack.toByteArray())))
        assertTrue(event.success); assertTrue(event.json.getBoolean("committed")); assertTrue(event.json.getBoolean("snapshot_pending"))
        val catalog = requireNotNull(parseFeedbackResultEvent(result(text = "{\"catalog\":[]}".toByteArray())))
        assertTrue(catalog.json.has("catalog")); assertFalse(catalog.json.has("message_id"))
    }
}
