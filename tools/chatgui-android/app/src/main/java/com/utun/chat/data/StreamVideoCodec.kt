package com.utun.chat.data

import android.media.MediaCodecInfo
import android.media.MediaFormat
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** IDs/MIME uAV2; аппаратный encoder name остаётся свойством режима устройства.
 * KEY: big-endian config length[4] + config + access unit, суммарно <=2 MiB, config <=64 KiB.
 * AVC/HEVC/VVC используют Annex B, VP9/AV1 — непрозрачные bytes; KEY AVC/HEVC проверяет наличие parameter sets.
 * Ошибки framing/ID бросаются caller, который логирует отказ кодека/потока; JNI/аппаратных проверок здесь нет. */
enum class StreamVideoCodec(val id: Int, val label: String, val mime: String, val profile: Int?, private val annexB: Boolean) {
    H264(1, "H.264", "video/avc", MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline, true),
    H265(2, "H.265", "video/hevc", MediaCodecInfo.CodecProfileLevel.HEVCProfileMain, true),
    VP9(3, "VP9", "video/x-vnd.on2.vp9", MediaCodecInfo.CodecProfileLevel.VP9Profile0, false),
    AV1(4, "AV1", "video/av01", MediaCodecInfo.CodecProfileLevel.AV1ProfileMain8, false),
    H266(5, "H.266", "video/vvc", null, true);

    companion object {
        const val MAX_CONFIG = 65536
        fun fromId(id: Int): StreamVideoCodec = values().firstOrNull { it.id == id } ?: error("Unsupported video codec id=$id")
    }
    fun normalize(bytes: ByteArray): ByteArray {
        require(bytes.isNotEmpty() && bytes.size <= H264AnnexB.MAX_FRAME)
        return if (annexB) H264AnnexB.normalize(bytes) else bytes.copyOf()
    }
    fun encoderProfile(available: IntArray): Int? = profile?.takeIf { it in available } ?: available.firstOrNull()
    /** Объединить CODEC_CONFIG одного запуска encoder, исключив одинаковые NAL bytes.
     * Caller сбрасывает previous при новой revision/encoder; для VP9/AV1 новый config заменяет прежний. */
    fun mergeConfig(previous: ByteArray, update: ByteArray): ByteArray {
        val next = normalize(update)
        val result = if (!annexB || previous.isEmpty()) next else {
            val units = H264AnnexB.nals(previous).map { it.second }.toMutableList()
            for ((_, unit) in H264AnnexB.nals(next)) if (units.none { it.contentEquals(unit) }) units += unit
            units.fold(byteArrayOf()) { a, unit -> a + unit }
        }
        require(result.size <= MAX_CONFIG)
        return result
    }
    private fun nalTypes(bytes: ByteArray): Set<Int> = H264AnnexB.nals(bytes).map { (_, nal) ->
        val at = if (nal[2] == 1.toByte()) 3 else 4
        when (this) {
            H264 -> nal[at].toInt() and 31
            H265 -> { require(nal.size > at + 1); (nal[at].toInt() ushr 1) and 63 }
            H266 -> { require(nal.size > at + 1); (nal[at + 1].toInt() ushr 3) and 31 }
            else -> error("NAL format expected")
        }
    }.toSet()

    fun keyFrame(frame: ByteArray, config: ByteArray): ByteArray {
        require(config.size <= MAX_CONFIG)
        val data = normalize(frame)
        if (this == H264) require(5 in nalTypes(data)) { "AVC KEY missing IDR" }
        if (this == H265) require(nalTypes(data).any { it in 16..21 }) { "HEVC KEY missing random access picture" }
        if (this == H266) require(nalTypes(data).any { it in 7..9 }) { "VVC KEY missing random access picture" }
        require(data.size + config.size + 4 <= H264AnnexB.MAX_FRAME)
        return ByteBuffer.allocate(4 + config.size + data.size).order(ByteOrder.BIG_ENDIAN)
            .putInt(config.size).put(config).put(data).array()
    }

    /** Конфигурация KEY отделена от access unit и может быть пустой для самодостаточного потока. */
    fun unpackKey(bytes: ByteArray): Pair<ByteArray, ByteArray> {
        require(bytes.size in 5..H264AnnexB.MAX_FRAME)
        val b = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        val count = b.int
        require(count in 0..MAX_CONFIG && count < b.remaining())
        val config = ByteArray(count).also { b.get(it) }; val frame = ByteArray(b.remaining()).also { b.get(it) }
        if (this == H264) require(nalTypes(config + frame).containsAll(setOf(5, 7, 8))) { "AVC KEY missing configuration" }
        if (this == H265) require(nalTypes(config + frame).containsAll(setOf(32, 33, 34))) { "HEVC KEY missing configuration" }
        return config to frame
    }

    fun decoderFormat(width: Int, height: Int, rotation: Int, config: ByteArray): MediaFormat =
        MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_ROTATION, rotation); setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, H264AnnexB.MAX_FRAME)
            if (config.isNotEmpty()) {
                if (this@StreamVideoCodec == H264) {
                    val nals = H264AnnexB.nals(config)
                    setByteBuffer("csd-0", ByteBuffer.wrap(nals.filter { it.first == 7 }.fold(byteArrayOf()) { a, n -> a + n.second }))
                    setByteBuffer("csd-1", ByteBuffer.wrap(nals.filter { it.first == 8 }.fold(byteArrayOf()) { a, n -> a + n.second }))
                } else setByteBuffer("csd-0", ByteBuffer.wrap(config))
            }
        }
}
