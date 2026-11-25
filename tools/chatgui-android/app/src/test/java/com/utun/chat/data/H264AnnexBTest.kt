package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class H264AnnexBTest {
    private val sps = byteArrayOf(0, 0, 0, 1, 0x67, 0x42, 0, 0x1f)
    private val pps = byteArrayOf(0, 0, 1, 0x68, 0x11)
    private val idr = byteArrayOf(0, 0, 0, 1, 0x65, 0x22, 0x33)

    @Test fun joinsConfigurationWithEveryKeyFrame() {
        val output = H264AnnexB.keyFrame(idr, sps + pps)
        assertEquals(listOf(7, 8, 5), H264AnnexB.nals(output).map { it.first })
        assertArrayEquals(output, H264AnnexB.keyFrame(output, sps + pps))
        assertArrayEquals(sps, H264AnnexB.nals(output).first().second)
    }

    @Test fun convertsLengthPrefixedAccessUnits() {
        val avcc = byteArrayOf(0, 0, 0, 3, 0x65, 0x22, 0x33, 0, 0, 0, 2, 0x61, 0x11)
        assertEquals(listOf(5, 1), H264AnnexB.nals(H264AnnexB.normalize(avcc)).map { it.first })
        assertArrayEquals(idr, H264AnnexB.normalize(avcc.copyOfRange(0, 7)))
    }

    @Test fun rejectsTruncatedAndMissingConfiguration() {
        for (data in listOf(byteArrayOf(), byteArrayOf(0, 0, 1), byteArrayOf(0, 0, 0, 5, 0x65),
            byteArrayOf(0x7f, -1, -1, -1, 0x65), byteArrayOf(0, 0, 0, 1, -1))) {
            assertThrows(IllegalArgumentException::class.java) { H264AnnexB.normalize(data) }
        }
        assertThrows(IllegalArgumentException::class.java) { H264AnnexB.keyFrame(idr, sps) }
        assertThrows(IllegalArgumentException::class.java) { H264AnnexB.keyFrame(byteArrayOf(0, 0, 1, 0x61, 0x11), sps + pps) }
        assertThrows(IllegalArgumentException::class.java) { H264AnnexB.normalize(ByteArray(H264AnnexB.MAX_FRAME + 1)) }
    }

    @Test fun lengthPrefixedNalUsesTheSameForbiddenBitValidationAsAnnexB() {
        val invalid = byteArrayOf(0, 0, 0, 2, -1, 1)
        assertThrows(IllegalArgumentException::class.java) { H264AnnexB.normalize(invalid) }
        assertThrows(IllegalArgumentException::class.java) { H264AnnexB.normalize(byteArrayOf(0, 0, 0, 1, -1, 1)) }
    }
}
