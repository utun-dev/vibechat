package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class StreamMetadataTest {
    @Test fun sourceKeysAndEpochsKeepTheirFullWidthAndEmptyNamesUseTheKey() {
        val address = "fc".repeat(32)
        val source = StreamSource.parse("""[{"id":"$address","epoch":"ffffffffffffffff","name":"","self":1}]""").single()
        assertEquals(address, source.id); assertEquals(-1L, source.epoch)
        assertEquals(address, source.displayName); assertTrue(source.self)
    }

    @Test fun sourceSnapshotsRejectZeroAndDuplicateIdentities() {
        val address = "01".repeat(32)
        for (json in listOf("""[{"id":"0","epoch":"1","name":"a","self":0}]""",
            """[{"id":"$address","epoch":"0","name":"a","self":0}]""",
            """[{"id":"$address","epoch":"a","name":"a","self":0},{"id":"$address","epoch":"b","name":"b","self":1}]""")) {
            assertThrows(IllegalArgumentException::class.java) { StreamSource.parse(json) }
        }
    }

    @Test fun transmitSnapshotMapsUnitsAndKeepsUnsignedPeerId() {
        val data = longArrayOf(100, 1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 2, 1, 1, 800000, -1, 0, 7, 5, 12, 18, 3)
        val stats = StreamTxStats.fromNative(42, data)
        assertEquals(100L, stats.sampleUs); assertEquals(8L, stats.connections); assertEquals(6L, stats.acked1s)
        assertTrue(stats.sourceOverloaded); assertEquals(800000, stats.videoBitrate)
        assertEquals(2, stats.buffer.total); assertEquals(StreamBufferPeer("f".repeat(16) + "0".repeat(31) + "7" + "0".repeat(15) + "5", 12, 18, 3), stats.buffer.peers.single())
        assertEquals(0f, stats.buffer.fraction(0), 0f); assertEquals(0.5f, stats.buffer.fraction(15), 0f)
        assertEquals(1f, stats.buffer.fraction(30), 0f); assertEquals(1f, StreamBufferPositions().fraction(0), 0f)
    }

    @Test fun malformedStatsCannotOverflowCountsOrReverseTheBufferWindow() {
        val valid = longArrayOf(100, 1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 2, 1, 0, 800000, -1, 0, 7, 5, 12, 18, 3)
        for (data in listOf(valid.copyOf(12), valid + 0, valid.copyOf().also { it[11] = Int.MAX_VALUE.toLong() + 1 },
            valid.copyOf().also { it[9] = 21 }, valid.copyOf().also { it[12] = 17 },
            valid.copyOf().also { it[20] = 11 }, valid.copyOf().also { it[21] = 256 },
            valid.copyOf().also { it[13] = 2 }, valid.copyOf().also { it[14] = Int.MAX_VALUE.toLong() + 1 })) {
            assertThrows(IllegalArgumentException::class.java) { StreamTxStats.fromNative(42, data) }
        }
    }

    @Test fun nativeTelemetryKeepsKnownUnknownAndLocalReserveSeparate() {
        val data = """{"self":"local","lag_ms":12300,"lag_valid":true,"waiting_live":true,"nodes":[{"name":"relay","flags":7,"us":2000000,"frames":4},{"name":"source","flags":0,"us":0,"frames":0}]}"""
        val stats = StreamPlaybackTelemetry.fromNative(data.toByteArray(), 500000, 1100)
        assertEquals(3, stats.nodes.size); assertEquals(2_000_000L, stats.nodes[0].durationUs)
        assertTrue(stats.nodes[0].stale); assertTrue(stats.nodes[0].down); assertNull(stats.nodes[1].durationUs)
        assertEquals(500_000L, stats.nodes[2].durationUs); assertEquals("1.1x", stats.speed)
        assertEquals(12300L, stats.liveLagMs); assertTrue(stats.waitingLive)
        val recovered = StreamPlaybackTelemetry.fromNative(data.replace("12300", "200").replace("\"waiting_live\":true", "\"waiting_live\":false")
            .toByteArray(), 500000, 1000)
        assertFalse(recovered.waitingLive); assertEquals(200L, recovered.liveLagMs)
        assertThrows(IllegalArgumentException::class.java) {
            StreamPlaybackTelemetry.fromNative(data.replace("\"lag_valid\":true", "\"lag_valid\":false").toByteArray(), null, 1000)
        }
    }
}
