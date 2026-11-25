package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

/** Контракт строкового глобального ID на границе Kotlin/JNI, включая отсутствие reply. */
class ChannelReplyTest {
    @Test fun quoteKeepsWhitespaceUnicodeAndDirection() {
        val text = "prefix  Выделено 😀\n  next line\nsuffix"
        val start = text.indexOf("  Выделено")
        val end = text.indexOf("\nsuffix")
        val expected = "  Выделено 😀\n  next line"
        assertEquals(expected, quoteFragment(text, start, end))
        assertEquals(expected, quoteFragment(text, end, start))
        val emoji = text.indexOf("😀")
        assertEquals("😀", quoteFragment(text, emoji, emoji + 1))
        assertEquals("😀", quoteFragment(text, emoji + 1, emoji + 2))
        for ((a, b) in listOf(0 to 0, -1 to 3, 0 to (text.length + 1))) assertNull(quoteFragment(text, a, b))
        assertNull(quoteFragment(" \n\t", 0, 3))
        assertNull(quoteFragment("a\u0000b", 0, 3))
    }

    @Test fun everyByteValueKeepsItsBits() {
        for (value in 0..255) {
            val id = "%02x".format(value).repeat(32)
            assertArrayEquals(ByteArray(32) { value.toByte() }, messageIdBytes(id))
            assertEquals(value != 0, validMessageId(id))
        }
    }

    @Test fun byteOrderMatchesGlobalId() {
        val id = "0123456789abcdef".repeat(4)
        val expected = byteArrayOf(1, 0x23, 0x45, 0x67, 0x89.toByte(), 0xab.toByte(), 0xcd.toByte(), 0xef.toByte())
        assertArrayEquals(ByteArray(32) { expected[it % expected.size] }, messageIdBytes(id))
    }

    @Test fun absentReplyAndZeroIdAreDistinctEncodingsWithoutRealMessage() {
        assertArrayEquals(byteArrayOf(), messageIdBytes(""))
        assertArrayEquals(ByteArray(32), messageIdBytes("0".repeat(64)))
        assertFalse(validMessageId(""))
        assertFalse(validMessageId("0".repeat(64)))
        assertTrue(validMessageId("0".repeat(63) + "1"))
    }

    @Test fun malformedNonemptyIdsAreRejectedWithoutNormalization() {
        for (id in listOf("1", "a".repeat(63), "a".repeat(65), "A".repeat(64), "g".repeat(64),
            "ａ".repeat(64), " " + "a".repeat(63), "a".repeat(63) + "\n")) {
            assertFalse(id, validMessageId(id))
            assertThrows(IllegalArgumentException::class.java) { messageIdBytes(id) }
        }
    }

    @Test fun returnedIdBytesBelongToCaller() {
        val id = "ff".repeat(32)
        val first = messageIdBytes(id)
        val second = messageIdBytes(id)
        first.fill(0)
        assertArrayEquals(ByteArray(32) { -1 }, second)
    }
}
