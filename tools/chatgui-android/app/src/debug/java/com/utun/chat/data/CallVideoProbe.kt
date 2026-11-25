package com.utun.chat.data

import android.app.Instrumentation
import android.graphics.ImageFormat
import android.media.Image
import android.media.ImageReader
import android.media.ImageWriter
import android.os.Bundle

/** Явная instrumentation-проба вне живого звонка; без camera capture и сетевой передачи.
 * codec=0/1/2 фильтрует encode/rotation проверки, rotations=1 проверяет YUV-углы всех режимов.
 * remote=call-video-*.idr декодирует один KEY из filesDir. Без remote/rotations проверяются bitrate и KEY VGA-режимов.
 * Результат finish=0 только после успешных проверок и cleanup; аппаратный запуск выполняется вручную. */
class CallVideoProbe : Instrumentation() {
    private var remote: String? = null
    private var selectedCodec = 0
    private var rotations = false
    override fun onCreate(arguments: Bundle?) {
        remote = arguments?.getString("remote"); selectedCodec = arguments?.getString("codec")?.toIntOrNull() ?: 0
        rotations = arguments?.getString("rotations") == "1"
        super.onCreate(arguments); start()
    }
    override fun onStart() {
        try {
            val decoders = CallVideoCodec.probeDecoders()
            remote?.let { name ->
                require(name.startsWith("call-video-") && '/' !in name)
                val codec = if (name.endsWith("-codec1.idr")) 1 else 2
                val bytes = java.io.File(targetContext.filesDir, name).readBytes()
                val actual = checkNotNull(CallVideoNative.inspect(codec, bytes))
                val format = CallVideoFormat(1, actual[0].toInt(), actual[1].toInt(), codec = codec,
                    level = actual[2].toInt(), profile = actual[3].toInt())
                val decoder = checkNotNull(decoders[codec]?.modes?.firstOrNull { it.supports(format) })
                CallVideoCodec.verifyDecode(format, List(4) { CallVideoCodec.Packet(1_000_000L + it * 33333, true, bytes) }, decoder.name)
                finish(0, Bundle().apply { putString("remote_decoded", "$name using ${decoder.name} format=$format") })
                return
            }
            val codecs = CallVideoCodec.probe()
            check(codecs.isNotEmpty() && decoders.isNotEmpty()) { "No verified encoders or decoders" }
            check(selectedCodec == 0 || selectedCodec in codecs) { "Requested codec unavailable: $selectedCodec" }
            for ((codec, entry) in codecs) {
                val decoder = decoders[codec] ?: continue
                val bytes = java.nio.ByteBuffer.allocate(64).order(java.nio.ByteOrder.BIG_ENDIAN)
                val size = entry.sizes.maxBy { it.width * it.height }
                bytes.position((codec - 1) * 16)
                bytes.putShort(size.width.toShort()).putShort(size.height.toShort()).putShort(30)
                    .putInt(size.width * size.height * 30).putInt(4_000_000).put(entry.level.toByte()).put(entry.profile.toByte())
                bytes.position(32 + (codec - 1) * 16)
                bytes.putShort(decoder.size.width.toShort()).putShort(decoder.size.height.toShort()).putShort(30)
                    .putInt(decoder.size.width * decoder.size.height * 30)
                    .putInt(4_000_000).put(decoder.level.toByte()).put(decoder.profiles.toByte())
                val caps = checkNotNull(CallVideoNative.rotatableCaps(bytes.array())) { "Verified CAPS rejected codec=$codec" }
                for (mode in entry.sizes) for (portrait in listOf(false, true)) {
                    val desired = CallVideoFormat(1, if (portrait) mode.height else mode.width,
                        if (portrait) mode.width else mode.height, codec = codec)
                    val selected = CallVideoNative.choose(caps, caps, desired.array()) ?: continue
                    check(CallVideoNative.supported(caps, caps, selected)) { "Chosen mode rejected $desired" }
                }
                android.util.Log.i("CallVideoProbe", "verified directed CAPS codec=$codec sizes=${entry.sizes}; both orientations agreed")
            }
            if (rotations) {
                CallVideoRotationProbe.run(codecs, decoders, selectedCodec)
                finish(0, Bundle().apply { putString("rotations", "All YUV corners, PTS and four rotations verified; codecs=${codecs.keys}") })
                return
            }
            var exercised = 0
            for ((codec, entry) in codecs) {
                if (selectedCodec != 0 && selectedCodec != codec) continue
                val mode = entry.modes.firstOrNull { it.size.width == 640 && it.size.height == 480 } ?: continue
                val f = CallVideoFormat(1, 640, 480, codec = codec, level = mode.level, profile = mode.profile)
                verifyRateChanges(f, mode.name)
                CallVideoCodec(f, 1_000_000, mode.name).use { encoder ->
                    encoder.prepare(); var key: ByteArray? = null; var input = 0
                    val deadline = System.nanoTime() + 3_000_000_000
                    while (key == null && System.nanoTime() < deadline) {
                        if (input < 8 && encoder.submit(null, 10_000_000L + input * 33333, input == 0)) input++
                        key = encoder.drain().firstOrNull { it.key }?.bytes; Thread.sleep(2)
                    }
                    java.io.File(targetContext.filesDir, "call-video-local-codec$codec.idr").writeBytes(checkNotNull(key))
                }
                exercised++
            }
            check(exercised > 0) { "No VGA encoder modes exercised for codec=$selectedCodec" }
            finish(0, Bundle().apply { putString("codecs", codecs.toString()); putString("decoders", decoders.toString()) })
        } catch (e: Exception) {
            android.util.Log.e("CallVideoProbe", "probe failed", e)
            finish(1, Bundle().apply { putString("error", e.toString()) })
        }
    }

    /** Шум требует заданную полосу: на однотонных пробах нельзя обнаружить проигнорированный bitrate. */
    private fun verifyRateChanges(format: CallVideoFormat, name: String) {
        ImageReader.newInstance(format.width, format.height, ImageFormat.YUV_420_888, 2).use { reader ->
            ImageWriter.newInstance(reader.surface, 1).use { writer ->
                writer.dequeueInputImage().use { image ->
                    CallVideoCodec(format, 4_000_000, name).use { encoder ->
                        encoder.prepare(); var random = 1
                        for ((phase, rate) in listOf(4_000_000, 300_000, 4_000_000).withIndex()) {
                            check(encoder.setBitrate(rate)) { "In-place bitrate unsupported: $name" }
                            var input = 0; var output = 0; var bytes = 0L; var prepared = false; var key = false
                            val base = 10_000_000L + phase * 4_000_000L
                            val started = System.nanoTime(); val deadline = started + 30_000_000_000L
                            while (output < 120 && System.nanoTime() < deadline) {
                                if (input < 120) {
                                    if (!prepared) { random = fillNoise(image, random); prepared = true }
                                    if (encoder.submit(image, base + input * 1_000_000L / 30, input % 30 == 0)) {
                                        input++; prepared = false
                                    }
                                }
                                for (packet in encoder.drain()) {
                                    check(packet.pts == base + output * 1_000_000L / 30) { "Lost/reordered rate-change frame" }
                                    if (output == 0) key = packet.key
                                    if (output >= 60) bytes += packet.bytes.size
                                    output++
                                }
                                Thread.sleep(2)
                            }
                            val measured = bytes * 8 / 2
                            val result = "name=$name phase=$phase target=$rate measured=$measured input=$input output=$output key=$key " +
                                "elapsed_ms=${(System.nanoTime() - started) / 1_000_000}"
                            android.util.Log.i("CallVideoProbe", "rate check $result")
                            check(input == 120 && output == 120 && key) { "Incomplete rate check: $result" }
                            // При сильной квантизации шум исчезает: low-rate кадр может занять меньше целевой полосы.
                            // Игнорирование команды ловит верхний предел, возврат к 4M — нижний предел high-rate фаз.
                            check(measured > 0 && measured < rate.toLong() * 3 / 2 && (rate == 300_000 || measured > rate / 2)) {
                                "Bitrate change ineffective: $result"
                            }
                        }
                    }
                }
            }
        }
    }

    private fun fillNoise(image: Image, seed: Int): Int {
        var random = seed
        for ((index, plane) in image.planes.withIndex()) {
            val width = image.width shr (if (index == 0) 0 else 1)
            val height = image.height shr (if (index == 0) 0 else 1)
            val buffer = plane.buffer.duplicate(); val start = buffer.position()
            val row = ByteArray((width - 1) * plane.pixelStride + 1) { 128.toByte() }
            for (y in 0 until height) {
                // Ограниченный контраст сохраняет сжимаемость тестового сигнала при низком bitrate.
                if (index == 0) for (x in 0 until width) {
                    random = random * 1664525 + 1013904223; row[x * plane.pixelStride] = (112 + (random ushr 27)).toByte()
                }
                val offset = start + y * plane.rowStride
                // Обе chroma-плоскости нейтральны: interleaved U/V тоже заполняем одним bulk put.
                buffer.position(offset); buffer.put(row)
            }
        }
        return random
    }
}
