package com.utun.chat.data

import android.graphics.ImageFormat
import android.media.Image
import android.media.ImageReader
import android.media.ImageWriter
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.util.Log
import android.util.Size
import kotlin.math.abs

/** Ручная часть CallVideoProbe: каждый выбранный codec/mode проверяет 12 кадров и четыре поворота.
 * Два encoder запуска покрывают 0/180 и 90/270°; decoder сверяет PTS, crop и YUV-углы с допуском 12.
 * Ошибка передаётся instrumentation; исходный YUV и codecs закрываются при любом результате. */
internal object CallVideoRotationProbe {
    private val corners = arrayOf(intArrayOf(0, 1, 2, 3), intArrayOf(2, 0, 3, 1), intArrayOf(3, 2, 1, 0), intArrayOf(1, 3, 0, 2))
    private val colors = arrayOf(intArrayOf(32, 80, 144, 208), intArrayOf(80, 100, 120, 140), intArrayOf(160, 180, 200, 220))

    fun run(codecs: Map<Int, CallVideoCodec.Companion.Encoder>, decoders: Map<Int, CallVideoCodec.Companion.Decoder>, selected: Int) {
        for ((codec, entry) in codecs) {
            if (selected != 0 && selected != codec) continue
            val decoder = checkNotNull(decoders[codec]) { "No decoder for rotation test codec=$codec" }
            for (mode in entry.modes) verify(codec, mode, decoder)
        }
    }

    private fun verify(codec: Int, entry: CallVideoCodec.Companion.EncoderMode, decoder: CallVideoCodec.Companion.Decoder) {
        val size = entry.size
        ImageReader.newInstance(size.width, size.height, ImageFormat.YUV_420_888, 2).use { reader ->
            ImageWriter.newInstance(reader.surface, 1).use { writer -> writer.dequeueInputImage().use { source ->
                fill(source)
                // Два направления одной геометрии идут через один encoder, как поворот телефона на 180°.
                for (quarter in 0..1) {
                    val format = CallVideoFormat(1, if (quarter == 0) size.width else size.height,
                        if (quarter == 0) size.height else size.width, codec = codec, level = entry.level, profile = entry.profile)
                    CallVideoCodec(format, 1_000_000, entry.name).use { encoder ->
                        encoder.prepare()
                        val packets = mutableListOf<CallVideoCodec.Packet>()
                        var sent = 0; var elapsed = 0L; var maximum = 0L
                        var waitUs = 0L; var copyUs = 0L; var queueUs = 0L
                        val deadline = System.nanoTime() + 8_000_000_000L
                        while (packets.size < 12 && System.nanoTime() < deadline) {
                            if (sent < 12) {
                                val rotation = (quarter + if (sent < 6) 0 else 2) * 90
                                val started = System.nanoTime()
                                if (encoder.submit(source, 10_000_000L + sent * 33333, sent == 0 || sent == 6, rotation)) {
                                    val us = (System.nanoTime() - started) / 1000
                                    elapsed += us; maximum = maxOf(maximum, us); sent++
                                    waitUs += encoder.inputWaitUs; copyUs += encoder.copyUs; queueUs += encoder.queueUs
                                }
                            }
                            packets += encoder.drain()
                            Thread.sleep(1)
                        }
                        check(sent == 12 && packets.size == 12 && packets.first().key) { "Incomplete rotated encoder output" }
                        val implementation = checkNotNull(decoder.modes.firstOrNull { it.supports(encoder.format) })
                        decode(encoder.format, packets, implementation.name, quarter)
                        val result = "codec=$codec name=${entry.name} source=$size output=${format.width}x${format.height} " +
                            "rotations=${quarter * 90}/${(quarter + 2) * 90} frames=$sent submitAvgUs=${elapsed / sent} submitMaxUs=$maximum " +
                            "waitAvgUs=${waitUs / sent} copyAvgUs=${copyUs / sent} queueAvgUs=${queueUs / sent}"
                        Log.i("CallVideoProbe", "rotation check $result")
                        check(elapsed / sent < 33333) { "Rotation cannot sustain 30fps: $result" }
                    }
                }
            } }
        }
    }

    private fun fill(image: Image) {
        for ((index, plane) in image.planes.withIndex()) {
            val shift = if (index == 0) 0 else 1
            val width = image.width shr shift; val height = image.height shr shift
            val buffer = plane.buffer.duplicate(); val start = buffer.position()
            for (y in 0 until height) for (x in 0 until width) {
                val quadrant = (if (y < height / 2) 0 else 2) + if (x < width / 2) 0 else 1
                buffer.put(start + y * plane.rowStride + x * plane.pixelStride, colors[index][quadrant].toByte())
            }
        }
    }

    private fun decode(format: CallVideoFormat, packets: List<CallVideoCodec.Packet>, name: String, quarter: Int) {
        val codec = MediaCodec.createByCodecName(name)
        VideoCodecResources(codec, name).use {
            val type = StreamVideoCodec.fromId(format.codec)
            val config = type.unpackKey(packets.first().bytes).first
            val media = CallVideoCodec.decoderFormat(format, codec, config).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
            }
            codec.configure(media, null, null, 0); codec.start()
            var sent = 0; var decoded = 0
            val info = MediaCodec.BufferInfo(); val deadline = System.nanoTime() + 8_000_000_000L
            while (decoded < packets.size && System.nanoTime() < deadline) {
                if (sent < packets.size) {
                    val input = codec.dequeueInputBuffer(0)
                    if (input >= 0) {
                        val packet = packets[sent++]
                        val bytes = if (packet.key) type.unpackKey(packet.bytes).let { it.first + it.second } else packet.bytes
                        codec.getInputBuffer(input)!!.put(bytes)
                        codec.queueInputBuffer(input, 0, bytes.size, packet.pts, 0)
                    }
                }
                val output = codec.dequeueOutputBuffer(info, 1000)
                if (output >= 0) {
                    try {
                        check(info.presentationTimeUs == packets[decoded].pts) { "Rotation reordered PTS" }
                        codec.getOutputImage(output).use { image ->
                            checkNotNull(image) { "Decoder has no readable YUV image" }
                            val crop = image.cropRect
                            check(crop.width() == format.width && crop.height() == format.height) { "Decoded geometry changed" }
                            val rotation = quarter + if (decoded < 6) 0 else 2
                            for ((index, plane) in image.planes.withIndex()) {
                                val shift = if (index == 0) 0 else 1
                                val w = crop.width() shr shift; val h = crop.height() shr shift
                                val buffer = plane.buffer.duplicate()
                                for (corner in 0..3) {
                                    val x = (crop.left shr shift) + w * (if (corner % 2 == 0) 1 else 3) / 4
                                    val y = (crop.top shr shift) + h * (if (corner < 2) 1 else 3) / 4
                                    val actual = buffer.get(buffer.position() + y * plane.rowStride + x * plane.pixelStride).toInt() and 255
                                    val expected = colors[index][corners[rotation][corner]]
                                    check(abs(actual - expected) <= 12) { "Rotated pixel mismatch frame=$decoded plane=$index corner=$corner actual=$actual expected=$expected" }
                                }
                            }
                        }
                        decoded++
                    } finally { codec.releaseOutputBuffer(output, false) }
                }
            }
            check(decoded == packets.size) { "Incomplete rotated decoder output $decoded/${packets.size}" }
        }
    }
}
