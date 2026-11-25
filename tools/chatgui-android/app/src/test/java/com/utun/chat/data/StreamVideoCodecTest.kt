package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class StreamVideoCodecTest {
    private fun nal(vararg bytes: Int) = byteArrayOf(0, 0, 0, 1) + bytes.map { it.toByte() }.toByteArray()
    @Test fun everyCodecCarriesConfigurationWithKey() {
        val configs = listOf(nal(0x67, 1) + nal(0x68, 2), nal(64, 1) + nal(66, 1) + nal(68, 1),
            byteArrayOf(1, 2), byteArrayOf(3, 4), nal(0, 121) + nal(0, 129))
        val frames = listOf(nal(0x65, 3), nal(38, 1), byteArrayOf(5, 6), byteArrayOf(7, 8), nal(0, 57))
        StreamVideoCodec.values().forEachIndexed { i, codec ->
            val packet = codec.keyFrame(frames[i], configs[i]); val decoded = codec.unpackKey(packet)
            assertArrayEquals(configs[i], decoded.first); assertArrayEquals(frames[i], decoded.second)
            assertEquals(codec, StreamVideoCodec.fromId(i + 1))
        }
    }
    @Test fun malformedConfigurationIsRejected() {
        for (size in 0..4) assertThrows(IllegalArgumentException::class.java) { StreamVideoCodec.VP9.unpackKey(ByteArray(size)) }
        assertThrows(IllegalArgumentException::class.java) { StreamVideoCodec.AV1.unpackKey(byteArrayOf(0, 1, 0, 1, 1)) }
        assertThrows(IllegalArgumentException::class.java) { StreamVideoCodec.H264.keyFrame(nal(0x41, 1), byteArrayOf()) }
        val decoded = StreamVideoCodec.VP9.unpackKey(StreamVideoCodec.VP9.keyFrame(byteArrayOf(1), byteArrayOf()))
        assertTrue(decoded.first.isEmpty()); assertArrayEquals(byteArrayOf(1), decoded.second)
    }
    @Test fun separateParameterSetsRemainCompleteWithoutDuplicates() {
        val sps = nal(0x67, 1); val pps = nal(0x68, 2)
        val config = StreamVideoCodec.H264.mergeConfig(sps, pps)
        assertArrayEquals(sps + pps, config)
        assertArrayEquals(config, StreamVideoCodec.H264.mergeConfig(config, sps))
        StreamVideoCodec.H264.unpackKey(StreamVideoCodec.H264.keyFrame(nal(0x65, 3), config))
    }
    @Test fun codecAndBitrateSurviveCameraModeSelection() {
        val base = StreamCaptureMode("a", 1280, 720, 30, 30, false, "encoder", 2_000_000)
        val hevc = base.copy(videoCodec = StreamVideoCodec.H265, minimumBitrate = 500000, maximumBitrate = 3000000)
        assertEquals(hevc, StreamCameraModes.choose(listOf(base, hevc), hevc))
        val config = StreamCaptureConfig(base, StreamOrientation.LEFT, 4000000).withMode(hevc)
        assertEquals(3000000, config.bitrate); assertEquals(StreamOrientation.LEFT, config.orientation)
    }
    @Test fun availableProfileIsUsedWhenPreferredProfileIsMissing() {
        val preferred = StreamVideoCodec.H264.profile!!
        assertEquals(preferred, StreamVideoCodec.H264.encoderProfile(intArrayOf(999, preferred)))
        assertEquals(999, StreamVideoCodec.H264.encoderProfile(intArrayOf(999)))
        assertNull(StreamVideoCodec.H266.encoderProfile(intArrayOf()))
    }
}
