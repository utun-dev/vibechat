package com.utun.chat.data

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer

/** AVC NAL framing: Annex B со start code 3/4 байта либо big-endian length[4] перед каждым NAL.
 * normalize копирует/конвертирует целый access unit <=2 MiB и проверяет границы/forbidden bit;
 * это не SPS/декодерная проверка bitstream. Ошибки аргументов бросаются владельцу для диагностики.
 * keyFrame возвращает обычный Annex B с SPS/PPS/IDR, без uAV2 config length из StreamVideoCodec. */
object H264AnnexB {
    const val MAX_FRAME = 2 * 1024 * 1024
    private val start = byteArrayOf(0, 0, 0, 1)

    fun normalize(bytes: ByteArray): ByteArray {
        require(bytes.isNotEmpty() && bytes.size <= MAX_FRAME)
        if (startSize(bytes, 0) != 0) {
            require(nals(bytes).isNotEmpty())
            return bytes.copyOf()
        }
        val out = ByteArrayOutputStream()
        val b = ByteBuffer.wrap(bytes)
        while (b.hasRemaining()) {
            require(b.remaining() >= 4)
            val length = b.int
            require(length > 0 && length <= b.remaining())
            require((bytes[b.position()].toInt() and 0x80) == 0)
            out.write(start); out.write(bytes, b.position(), length); b.position(b.position() + length)
        }
        require(out.size() <= MAX_FRAME)
        return out.toByteArray()
    }

    private fun startSize(data: ByteArray, at: Int): Int {
        if (at + 3 <= data.size && data[at] == 0.toByte() && data[at + 1] == 0.toByte()) {
            if (data[at + 2] == 1.toByte()) return 3
            if (at + 4 <= data.size && data[at + 2] == 0.toByte() && data[at + 3] == 1.toByte()) return 4
        }
        return 0
    }

    /** Полные NAL вместе со start code; SPS/PPS можно использовать как MediaCodec csd-0/csd-1. */
    fun nals(bytes: ByteArray): List<Pair<Int, ByteArray>> {
        val result = mutableListOf<Pair<Int, ByteArray>>()
        var at = 0
        while (at < bytes.size) {
            val prefix = startSize(bytes, at)
            require(prefix != 0 && at + prefix < bytes.size)
            var end = at + prefix + 1
            while (end < bytes.size && startSize(bytes, end) == 0) end++
            require((bytes[at + prefix].toInt() and 0x80) == 0)
            result += (bytes[at + prefix].toInt() and 31) to bytes.copyOfRange(at, end)
            at = end
        }
        return result
    }

    fun keyFrame(frame: ByteArray, config: ByteArray): ByteArray {
        val normalized = normalize(frame)
        val types = nals(normalized).map { it.first }.toSet()
        require(5 in types)
        val output = if (7 in types && 8 in types) normalized else config + normalized
        val all = nals(output).map { it.first }.toSet()
        require(7 in all && 8 in all && output.size <= MAX_FRAME)
        return output
    }
}
