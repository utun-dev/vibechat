package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer

class StreamReadQueueTest {
    private fun record(kind: Int, pts: Long = 0, bytes: ByteArray = byteArrayOf()): ByteArray =
        ByteBuffer.allocate(28 + bytes.size).apply {
            put(0, kind.toByte()); putLong(8, pts); putInt(16, bytes.size); position(28); put(bytes)
        }.array()

    @Test fun videoEndPassesBlockedAudioButPreservesVideoOrder() {
        val queue = StreamReadQueue()
        queue.add(record(2, 20) + record(1, 40) + record(7) + record(8) + record(5))
        assertNull(queue.take(video = false, audio = false))
        assertEquals(1, queue.take(video = true, audio = false)!![0].toInt())
        assertEquals(7, queue.take(video = false, audio = false)!![0].toInt())
        assertNull(queue.take(video = false, audio = false))
        assertEquals(2, queue.take(video = false, audio = true)!![0].toInt())
        assertEquals(8, queue.take(video = false, audio = false)!![0].toInt())
        assertEquals(5, queue.take(video = false, audio = false)!![0].toInt())
        assertEquals(0, queue.size)
    }

    @Test fun audioEndPassesBlockedVideo() {
        val queue = StreamReadQueue()
        queue.add(record(1) + record(2) + record(8) + record(7) + record(5))
        assertEquals(2, queue.take(video = false, audio = true)!![0].toInt())
        assertEquals(8, queue.take(video = false, audio = false)!![0].toInt())
        assertNull(queue.take(video = false, audio = false))
        assertEquals(1, queue.take(video = true, audio = false)!![0].toInt())
        assertEquals(7, queue.take(video = false, audio = false)!![0].toInt())
        assertEquals(5, queue.take(video = false, audio = false)!![0].toInt())
    }

    @Test fun gapAndCloseImmediatelyCancelBlockedTracks() {
        for (kind in listOf(3, 6)) {
            val queue = StreamReadQueue()
            queue.add(record(1) + record(2))
            queue.add(record(kind))
            assertEquals(kind, queue.take(video = false, audio = false)!![0].toInt())
            assertEquals(0, queue.size)
        }
    }

    @Test fun variablePayloadsKeepRecordBoundaries() {
        val queue = StreamReadQueue()
        val video = record(1, 42, byteArrayOf(1, 2, 3))
        val audio = record(2, 43, byteArrayOf(4))
        assertEquals(2, queue.add(video + audio))
        assertArrayEquals(audio, queue.take(video = false, audio = true))
        assertArrayEquals(video, queue.take(video = true, audio = false))
    }

    @Test fun malformedTailCannotApplyItsEarlierGapOrReplacePendingData() {
        val queue = StreamReadQueue(); val original = record(1, 42)
        queue.add(original)
        for (invalid in listOf(record(3) + byteArrayOf(1), record(6) + record(9),
            record(2) + record(1).also { ByteBuffer.wrap(it).putInt(16, 100) })) {
            assertThrows(IllegalStateException::class.java) { queue.add(invalid) }
            assertEquals(1, queue.size)
        }
        assertArrayEquals(original, queue.take(video = true, audio = true))
    }

    @Test fun excessiveRecordCountIsRejectedBeforeAnyChange() {
        val queue = StreamReadQueue(); val original = record(2, 123)
        queue.add(original)
        val invalid = List(17) { record(3) }.fold(byteArrayOf()) { bytes, next -> bytes + next }
        assertThrows(IllegalStateException::class.java) { queue.add(invalid) }
        assertEquals(1, queue.size); assertArrayEquals(original, queue.take(video = true, audio = true))
    }
}
