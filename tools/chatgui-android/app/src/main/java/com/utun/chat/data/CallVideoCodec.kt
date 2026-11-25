package com.utun.chat.data

import android.media.Image
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.util.Size
import java.util.concurrent.locks.ReentrantLock

/** Codec принадлежит одному worker; close идемпотентен, при отказе сохраняет ресурсы для retry.
 * YUV input отделён от камеры: замена на IDR не меняет capture PTS. Input Image принадлежит resources.
 */
internal class CallVideoCodec(requested: CallVideoFormat, bitrate: Int, val name: String) : AutoCloseable {
    var bitrate = bitrate
        private set
    var format = requested
        private set
    data class Packet(val pts: Long, val key: Boolean, val bytes: ByteArray)
    private data class InputTime(val codecPts: Long, val capturePts: Long)
    private val type = StreamVideoCodec.fromId(format.codec)
    private val resources = VideoCodecResources(MediaCodec.createByCodecName(name), name)
    private val codec = resources.codec
    private var config = byteArrayOf()
    private val partial = java.io.ByteArrayOutputStream()
    private var partialPts = -1L
    private var partialFlags = 0
    private var preparing = false
    private var lastInput = -1L
    private var submitted = 0L
    private val inputTimes = ArrayDeque<InputTime>()
    val pendingFrames get() = inputTimes.size
    var inputWaitUs = 0L
        private set
    var copyUs = 0L
        private set
    var queueUs = 0L
        private set

    init {
        try {
            val media = MediaFormat.createVideoFormat(type.mime, format.width, format.height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
                setInteger(MediaFormat.KEY_PROFILE, if (format.codec == 1) {
                    if (format.profile == 1) MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline
                    else MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedBaseline
                }
                    else MediaCodecInfo.CodecProfileLevel.HEVCProfileMain)
                // Запрос Main tier одинаков в probe/live; Qualcomm может выдать High, фактический режим проверяет prepare().
                setInteger(MediaFormat.KEY_LEVEL, platformLevel(format.codec, format.level))
                setInteger(MediaFormat.KEY_BIT_RATE, bitrate); setInteger(MediaFormat.KEY_FRAME_RATE, format.fps)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, -1)
                setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
                if (Build.VERSION.SDK_INT >= 29) setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
                setInteger(MediaFormat.KEY_LATENCY, 0)
            }
            codec.configure(media, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE); codec.start()
            LogManager.addLog("INFO", "call", "video encoder open name=$name format=$format bitrate=$bitrate actual=${codec.inputFormat}")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "call", "video encoder initialization failed name=$name format=$format: $e")
            try { resources.close() } catch (cleanup: Exception) { e.addSuppressed(cleanup) }
            throw e
        }
    }

    fun submit(image: Image?, pts: Long, key: Boolean, rotation: Int = 0): Boolean {
        check(pts > lastInput) { "Encoder input PTS reversed name=$name pts=$pts previous=$lastInput" }
        val started = System.nanoTime()
        val index = codec.dequeueInputBuffer(0)
        if (index < 0) return false
        if (key) codec.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        val input = resources.own(checkNotNull(codec.getInputImage(index)) { "Encoder lacks flexible YUV input" })
        val acquired: Long
        val copied: Long
        try {
            check(input.width >= format.width && input.height >= format.height)
            val crop = image?.cropRect
            if (crop != null) {
                check(crop.left % 2 == 0 && crop.top % 2 == 0)
                check((if (rotation % 180 == 0) crop.width() else crop.height()) == format.width
                    && (if (rotation % 180 == 0) crop.height() else crop.width()) == format.height) { "YUV input geometry mismatch" }
            }
            acquired = System.nanoTime()
            for (plane in 0..2) {
                val w = format.width shr (if (plane == 0) 0 else 1)
                val h = format.height shr (if (plane == 0) 0 else 1)
                val dst = input.planes[plane]
                if (image?.format == android.graphics.PixelFormat.RGBA_8888) {
                    val pixels = image.planes.single()
                    val source = pixels.buffer.duplicate().apply {
                        val region = image.cropRect
                        position(position() + region.top * pixels.rowStride + region.left * pixels.pixelStride)
                    }.slice()
                    check(CallVideoNative.rgbaPlane(source, pixels.rowStride, pixels.pixelStride,
                        dst.buffer.slice(), dst.rowStride, dst.pixelStride, format.width, format.height, plane)) { "RGBA conversion failed" }
                    continue
                }
                val src = image?.planes?.get(plane)
                if (src != null && crop != null) {
                    val shift = if (plane == 0) 0 else 1
                    val source = src.buffer.duplicate().apply {
                        position(position() + (crop.top shr shift) * src.rowStride + (crop.left shr shift) * src.pixelStride)
                    }.slice()
                    check(CallVideoNative.rotatePlane(source, src.rowStride, src.pixelStride, dst.buffer.slice(), dst.rowStride,
                        dst.pixelStride, crop.width() shr shift, crop.height() shr shift, rotation)) { "YUV plane rotation failed" }
                    continue
                }
                val row = ByteArray(w)
                for (y in 0 until h) {
                    row.fill(if (plane == 0) 64 else 128.toByte())
                    val buffer = dst.buffer.duplicate(); val start = buffer.position() + y * dst.rowStride
                    if (dst.pixelStride == 1) { buffer.position(start); buffer.put(row) }
                    else for (x in 0 until w) buffer.put(start + x * dst.pixelStride, row[x])
                }
            }
            copied = System.nanoTime()
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "call", "video YUV input failed name=$name pts=$pts rotation=$rotation: $e")
            try { resources.closeImage(input) } catch (cleanup: Exception) { e.addSuppressed(cleanup) }
            throw e
        }
        resources.closeImage(input)
        // После затора бюджет кодера остаётся на один кадр. Исходный PTS нужен для доставки и A/V.
        val codecPts = submitted * 1_000_000L / format.fps
        codec.queueInputBuffer(index, 0, format.width * format.height * 3 / 2, codecPts, 0)
        inputWaitUs = (acquired - started) / 1000; copyUs = (copied - acquired) / 1000
        queueUs = (System.nanoTime() - copied) / 1000
        inputTimes.addLast(InputTime(codecPts, pts))
        lastInput = pts; submitted++; return true
    }

    /** Меняет битрейт перед следующей подачей кадра, сохраняя ссылки на предыдущие кадры. */
    fun setBitrate(value: Int): Boolean {
        if (value == bitrate) return true
        return try {
            codec.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, value) })
            LogManager.addLog("INFO", "call", "video bitrate updated without restart name=$name old=$bitrate target=$value")
            bitrate = value; true
        } catch (e: Exception) {
            LogManager.addLog("WARN", "call", "video live bitrate refused name=$name target=$value: ${e.message}"); false
        }
    }

    fun drain(): List<Packet> {
        val packets = mutableListOf<Packet>(); val info = MediaCodec.BufferInfo()
        repeat(16) {
            val index = codec.dequeueOutputBuffer(info, 0)
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                val f = codec.outputFormat
                config = listOf("csd-0", "csd-1").mapNotNull { f.getByteBuffer(it)?.duplicate()?.let { b ->
                    ByteArray(b.remaining()).also(b::get).let(type::normalize)
                } }.fold(byteArrayOf()) { a, b -> a + b }
                check(config.size <= StreamVideoCodec.MAX_CONFIG)
                LogManager.addLog("INFO", "call", "video encoder output format=$f config=${config.size}")
                return@repeat
            }
            if (index < 0) return packets
            try {
                if (info.size == 0) return@repeat
                val b = checkNotNull(codec.getOutputBuffer(index)).duplicate()
                b.position(info.offset); b.limit(info.offset + info.size)
                val bytes = ByteArray(info.size).also(b::get)
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                    config = type.mergeConfig(config, bytes); return@repeat
                }
                if (partial.size() == 0) { partialPts = info.presentationTimeUs; partialFlags = 0 }
                check(partialPts == info.presentationTimeUs && partial.size() + bytes.size <= H264AnnexB.MAX_FRAME)
                partial.write(bytes); partialFlags = partialFlags or info.flags
                if (info.flags and MediaCodec.BUFFER_FLAG_PARTIAL_FRAME != 0) return@repeat
                val data = type.normalize(partial.toByteArray()); partial.reset()
                val key = partialFlags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                val output = if (key) type.keyFrame(data, config) else data
                val input = checkNotNull(inputTimes.removeFirstOrNull()) { "Encoder output without input name=$name" }
                check(info.presentationTimeUs == input.codecPts) {
                    "Encoder reordered PTS name=$name output=${info.presentationTimeUs} expected=${input.codecPts} capture=${input.capturePts}"
                }
                if (key) {
                    if (preparing) {
                        val actual = checkNotNull(CallVideoNative.inspect(format.codec, output)) { "Invalid encoder bitstream" }
                        check(actual[0] == format.width.toLong() && actual[1] == format.height.toLong()) { "Encoder changed geometry" }
                        format = format.copy(level = actual[2].toInt(), profile = actual[3].toInt())
                    }
                    validateKey(output, format)
                }
                packets += Packet(input.capturePts, key, output)
            } finally { codec.releaseOutputBuffer(index, false) }
        }
        return packets
    }
    /** Забрать все принятые входы перед заменой: EOS не требует новых кадров камеры. */
    fun finish(): List<Packet> {
        val packets = mutableListOf<Packet>(); val started = System.nanoTime(); var eos = false
        while (pendingFrames > 0 && System.nanoTime() - started < 2_000_000_000) {
            if (!eos) {
                val input = codec.dequeueInputBuffer(0)
                if (input >= 0) {
                    codec.queueInputBuffer(input, 0, 0, submitted * 1_000_000L / format.fps, MediaCodec.BUFFER_FLAG_END_OF_STREAM); eos = true
                }
            }
            packets += drain()
            if (pendingFrames > 0) Thread.sleep(1)
        }
        check(pendingFrames == 0) { "Encoder drain timeout name=$name pending=$pendingFrames" }
        LogManager.addLog("DEBUG", "call", "video encoder drained name=$name frames=${packets.size} us=${(System.nanoTime() - started) / 1000}")
        return packets
    }
    override fun close() = resources.close()
    /** Проверяем фактический режим до публикации; пробные кадры не уходят в звонок. */
    fun prepare(keepRunning: () -> Boolean = { true }) {
        preparing = true
        try {
            var input = 0; var output = 0; var key = false
            val deadline = System.nanoTime() + 5_000_000_000
            while (output < 8 && System.nanoTime() < deadline && keepRunning()) {
                if (input < 8 && submit(null, 1_000_000L + input * 1_000_000L / format.fps, input == 0)) input++
                for (packet in drain()) { if (output == 0) key = packet.key; output++ }
                Thread.sleep(2)
            }
            check(keepRunning()) { "Encoder probe cancelled" }
            check(input == 8 && output == 8 && key) { "Encoder probe incomplete inputs=$input outputs=$output IDR=$key" }
            LogManager.addLog("INFO", "call", "video encoder verified name=$name actual=$format bitrate=$bitrate")
        } finally { preparing = false }
    }

    companion object {
        private val encoderProbeLock = ReentrantLock()
        private val decoderProbeLock = ReentrantLock()
        @Volatile private var verifiedEncoders: Map<Int, Encoder>? = null
        @Volatile private var verifiedDecoders: Map<Int, Decoder>? = null
        fun platformLevel(codec: Int, level: Int): Int = if (codec == 1) when (level) {
            30 -> MediaCodecInfo.CodecProfileLevel.AVCLevel3
            31 -> MediaCodecInfo.CodecProfileLevel.AVCLevel31
            32 -> MediaCodecInfo.CodecProfileLevel.AVCLevel32
            40 -> MediaCodecInfo.CodecProfileLevel.AVCLevel4
            42 -> MediaCodecInfo.CodecProfileLevel.AVCLevel42
            else -> error("Unsupported AVC level $level")
        } else when (level) {
            90 -> MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel3
            93 -> MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel31
            120 -> MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel4
            123 -> MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel41
            else -> error("Unsupported HEVC level $level")
        }
        data class EncoderMode(val size: Size, val name: String, val profile: Int, val level: Int, val fps: Int = 30)
        data class Encoder(val modes: List<EncoderMode>) {
            val sizes get() = modes.map { it.size }.distinct()
            val fps get() = modes.maxOf { it.fps }
            val pixelsPerSecond get() = modes.maxOf { it.size.width * it.size.height * it.fps }
            val profile get() = modes.fold(0) { bits, mode -> bits or mode.profile }
            val level get() = modes.maxOf { it.level }
        }
        data class DecoderMode(val size: Size, val name: String, val profiles: Int, val level: Int, val pipeline: Int, val fps: Int = 30) {
            fun supports(f: CallVideoFormat) = profiles and f.profile != 0 && f.level <= level && f.fps <= fps &&
                f.width.toLong() * f.height * f.fps <= size.width.toLong() * size.height * fps &&
                maxOf(f.width, f.height) <= maxOf(size.width, size.height) && f.width.toLong() * f.height <= size.width.toLong() * size.height
        }
        data class Decoder(val modes: List<DecoderMode>) {
            val size get() = modes.maxBy { it.size.width * it.size.height }.size
            val profiles get() = modes.fold(0) { bits, mode -> bits or mode.profiles }
            val level get() = modes.maxOf { it.level }
            val fps get() = modes.maxOf { it.fps }
            val pixelsPerSecond get() = modes.maxOf { it.size.width * it.size.height * it.fps }
        }
        /** Аппаратные реализации проверяются первыми; software остаётся резервом после реальной пробы. */
        private fun candidates(encoder: Boolean) = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { it.isEncoder == encoder }.sortedBy {
                if (Build.VERSION.SDK_INT >= 29) { if (it.isHardwareAccelerated) 0 else 1 }
                else if (it.name.startsWith("OMX.google.") || it.name.startsWith("c2.android.")) 1 else 0
            }
        /** Тот же opt-in low latency, что у viewer стримов; применяется и в пробе, и в live. */
        fun decoderFormat(f: CallVideoFormat, decoder: MediaCodec, config: ByteArray): MediaFormat {
            val type = StreamVideoCodec.fromId(f.codec)
            val media = type.decoderFormat(f.width, f.height, f.rotation, config)
            val lowLatency = Build.VERSION.SDK_INT >= 30 && decoder.codecInfo.getCapabilitiesForType(type.mime)
                .isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)
            if (lowLatency) media.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            LogManager.addLog("INFO", "call", "video decoder configure name=${decoder.name} lowLatency=$lowLatency format=$f")
            return media
        }
        /** Encode и decode независимы: ошибка собственного encoder не запрещает приём. */
        fun probe(keepRunning: () -> Boolean = { true }): Map<Int, Encoder> {
            if (!keepRunning()) return emptyMap()
            verifiedEncoders?.let {
                LogManager.addLog("INFO", "call", "video encoder capabilities cache hit codecs=${it.size}")
                return it
            }
            while (!encoderProbeLock.tryLock()) {
                if (!keepRunning()) { LogManager.addLog("INFO", "call", "video encoder capability wait cancelled"); return emptyMap() }
                Thread.sleep(5)
            }
            try {
                if (!keepRunning()) return emptyMap()
                verifiedEncoders?.let { return it }
                val started = System.nanoTime()
                LogManager.addLog("INFO", "call", "video encoder capability verification begin")
                val result = mutableMapOf<Int, Encoder>()
                for (type in listOf(StreamVideoCodec.H265, StreamVideoCodec.H264)) {
                    val modes = mutableListOf<EncoderMode>()
                    for (size in listOf(Size(640, 480), Size(1280, 720), Size(1920, 1080))) {
                      for (fps in listOf(30, 50, 60)) {
                        if (!keepRunning()) return emptyMap()
                        for (entry in candidates(true)) {
                            if (entry.supportedTypes.none { it == type.mime }) continue
                            try {
                                val caps = entry.getCapabilitiesForType(type.mime)
                                if (caps.encoderCapabilities?.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR) != true) continue
                                var foundProfile = 0; var foundLevel = 0
                                for ((w, h) in listOf(size.width to size.height, size.height to size.width)) {
                                    check(caps.videoCapabilities?.areSizeAndRateSupported(w, h, fps.toDouble()) == true) { "Geometry/rate unsupported: ${w}x$h@$fps" }
                                    val fullhd = size.width > 1280
                                    val f = CallVideoFormat(1, w, h, fps = fps, codec = type.id,
                                        level = if (type.id == 1) { if (fullhd) { if (fps > 30) 42 else 40 } else { if (fps > 30) 32 else 31 } }
                                            else { if (fullhd) { if (fps > 30) 123 else 120 } else { if (fps > 30) 120 else 93 } },
                                        profile = if (type.id == 1) 2 else 1)
                                    var verified: CallVideoFormat? = null
                                    for (level in (if (type.id == 2) listOf(f.level, maxOf(f.level, 120)) else listOf(f.level)).distinct()) {
                                        if (!keepRunning()) return emptyMap()
                                        try {
                                            val actual = CallVideoCodec(f.copy(level = level), 1_000_000, entry.name).use { encoder ->
                                                encoder.prepare(keepRunning); encoder.format
                                            }
                                            verified = actual // Публикуем только после подтверждённого close пробного codec.
                                            break
                                        } catch (e: Exception) {
                                            LogManager.addLog("WARN", "call", "video encoder level probe rejected name=${entry.name} size=${w}x$h level=$level: ${e.message}")
                                        }
                                    }
                                    val actual = checkNotNull(verified) { "No valid encoder level: ${entry.name} ${w}x$h" }
                                    check(foundProfile == 0 || foundProfile == actual.profile) { "Geometry changes profile/tier" }
                                    foundProfile = actual.profile; foundLevel = maxOf(foundLevel, actual.level)
                                }
                                modes += EncoderMode(size, entry.name, foundProfile, foundLevel, fps)
                                LogManager.addLog("INFO", "call", "video codec probe passed type=${type.label} encoder=${entry.name} size=$size fps=$fps"); break
                            } catch (e: Exception) { LogManager.addLog("WARN", "call", "video codec probe rejected ${entry.name} size=$size fps=$fps: ${e.message}") }
                        }
                      }
                    }
                    if (modes.isNotEmpty()) result[type.id] = Encoder(modes)
                }
                if (!keepRunning()) return emptyMap()
                if (result.isNotEmpty()) verifiedEncoders = result.toMap()
                LogManager.addLog("INFO", "call", "video encoder capabilities verified codecs=${result.size} modes=${result.values.sumOf { it.modes.size }} elapsedMs=${(System.nanoTime() - started) / 1_000_000}")
                return result
            } finally { encoderProbeLock.unlock() }
        }
        fun probeDecoders(keepRunning: () -> Boolean = { true }): Map<Int, Decoder> {
            if (!keepRunning()) return emptyMap()
            verifiedDecoders?.let {
                LogManager.addLog("INFO", "call", "video decoder capabilities cache hit codecs=${it.size}")
                return it
            }
            while (!decoderProbeLock.tryLock()) {
                if (!keepRunning()) { LogManager.addLog("INFO", "call", "video decoder capability wait cancelled"); return emptyMap() }
                Thread.sleep(5)
            }
            try {
                if (!keepRunning()) return emptyMap()
                verifiedDecoders?.let { return it }
                val started = System.nanoTime()
                LogManager.addLog("INFO", "call", "video decoder capability verification begin")
                val result = mutableMapOf<Int, Decoder>()
                for (type in listOf(StreamVideoCodec.H265, StreamVideoCodec.H264)) {
                    val modes = mutableListOf<DecoderMode>()
                    for (size in listOf(Size(1280, 720), Size(1920, 1080))) {
                      for (fps in listOf(30, 50, 60)) {
                        if (!keepRunning()) return emptyMap()
                        for (entry in candidates(false)) {
                            if (entry.supportedTypes.none { it == type.mime }) continue
                            try {
                                val caps = entry.getCapabilitiesForType(type.mime)
                                if (caps.videoCapabilities?.areSizeAndRateSupported(size.width, size.height, fps.toDouble()) != true) continue
                                var profiles = 0; var pipeline = 0; var maximumLevel = 0
                                for (profile in listOf(1, 2)) {
                                    if (!keepRunning()) return emptyMap()
                                    try {
                                        val level = if (type.id == 1) { if (size.width > 1280) { if (fps > 30) 42 else 40 } else { if (fps > 30) 32 else 31 } }
                                            else if (size.width > 1280) { if (fps > 30) 123 else 120 } else if (fps > 30 || profile == 2) 120 else 93
                                        var profilePipeline = 0
                                        for (portrait in listOf(false, true)) {
                                            val w = if (portrait) size.height else size.width; val h = if (portrait) size.width else size.height
                                            check(caps.videoCapabilities?.areSizeAndRateSupported(w, h, fps.toDouble()) == true) {
                                                "Decoder geometry unsupported: ${w}x$h"
                                            }
                                            val f = CallVideoFormat(1, w, h, fps = fps, codec = type.id, level = level, profile = profile)
                                            val key = checkNotNull(CallVideoNative.probeKey(type.id, profile, w, h, if (fps > 30) 60 else 30))
                                            validateKey(key, f)
                                            profilePipeline = maxOf(profilePipeline,
                                                verifyDecode(f, List(8) { Packet(1_000_000L + it * 1_000_000L / fps, true, key) },
                                                    entry.name, true, keepRunning))
                                        }
                                        profiles = profiles or profile; maximumLevel = maxOf(maximumLevel, level)
                                        pipeline = maxOf(pipeline, profilePipeline)
                                    } catch (e: Exception) {
                                        LogManager.addLog("WARN", "call", "video decoder profile probe rejected name=${entry.name} " +
                                            "codec=${type.id} profile=$profile size=$size: ${e.message}")
                                    }
                                }
                                check(profiles != 0) { "No verified decoder profiles" }
                                modes += DecoderMode(size, entry.name, profiles, maximumLevel, pipeline, fps)
                                LogManager.addLog("INFO", "call", "video decoder verified codec=${type.id} name=${entry.name} size=$size fps=$fps profiles=$profiles pipeline=$pipeline")
                                break
                            } catch (e: Exception) { LogManager.addLog("WARN", "call", "video decoder probe rejected ${entry.name} size=$size: ${e.message}") }
                        }
                      }
                    }
                    if (modes.isNotEmpty()) result[type.id] = Decoder(modes)
                }
                if (!keepRunning()) return emptyMap()
                if (result.isNotEmpty()) verifiedDecoders = result.toMap()
                LogManager.addLog("INFO", "call", "video decoder capabilities verified codecs=${result.size} modes=${result.values.sumOf { it.modes.size }} elapsedMs=${(System.nanoTime() - started) / 1_000_000}")
                return result
            } finally { decoderProbeLock.unlock() }
        }
        internal fun verifyDecode(f: CallVideoFormat, packets: List<Packet>, name: String, paced: Boolean = false,
            keepRunning: () -> Boolean = { true }): Int {
            val type = StreamVideoCodec.fromId(f.codec)
            val config = type.unpackKey(packets.first().bytes).first
            return VideoCodecResources(MediaCodec.createByCodecName(name), name).use { resources ->
                val decoder = resources.codec
                decoder.configure(decoderFormat(f.copy(rotation = 0), decoder, config), null, null, 0); decoder.start()
                var sent = 0; var decoded = 0; var pipeline = 0
                val info = MediaCodec.BufferInfo(); val started = System.nanoTime(); val deadline = started + 2_000_000_000
                while (decoded < packets.size && System.nanoTime() < deadline && keepRunning()) {
                    // Измеряем pipeline с исходной частотой до первого выхода, затем проверяем весь хвост без пауз.
                    if (sent < packets.size && (!paced || decoded > 0 || System.nanoTime() - started >= sent * 1_000_000_000L / f.fps)) {
                        val i = decoder.dequeueInputBuffer(0)
                        if (i >= 0) {
                            val p = packets[sent++]; val data = if (p.key) type.unpackKey(p.bytes).second else p.bytes
                            decoder.getInputBuffer(i)!!.put(data); decoder.queueInputBuffer(i, 0, data.size, p.pts, 0)
                        }
                    }
                    val out = decoder.dequeueOutputBuffer(info, 1000)
                    if (out >= 0) {
                        if (decoded == 0) pipeline = sent - 1
                        check(decoded < packets.size && info.presentationTimeUs == packets[decoded].pts) { "Decoder reordered PTS" }
                        val actual = decoder.outputFormat
                        check(actual.getInteger(MediaFormat.KEY_WIDTH) >= f.width && actual.getInteger(MediaFormat.KEY_HEIGHT) >= f.height)
                        decoded++; decoder.releaseOutputBuffer(out, false)
                    }
                }
                check(keepRunning()) { "Decoder probe cancelled" }
                check(decoded == packets.size) { "Decoder did not return all probe frames $decoded/${packets.size}" }
                if (paced) LogManager.addLog("DEBUG", "call", "video decoder probe name=$name inputs=$sent outputs=$decoded " +
                    "pipeline=$pipeline elapsedMs=${(System.nanoTime() - started) / 1_000_000}")
                pipeline
            }
        }

        /** Единый C validator используется также ChatGUI. */
        fun validateKey(bytes: ByteArray, f: CallVideoFormat) {
            val s = checkNotNull(CallVideoNative.inspect(f.codec, bytes)) { "Invalid video KEY bitstream" }
            check(s[0] == f.width.toLong() && s[1] == f.height.toLong() && s[2] <= f.level
                && (s[3] == f.profile.toLong() || (f.codec == 1 && f.profile == 1 && s[3] == 2L))) {
                "Video KEY mismatch actual=${s.contentToString()} expected=$f"
            }
        }
    }
}
