package com.utun.chat.data

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Looper
import android.opengl.EGL14
import android.opengl.EGLExt
import android.opengl.GLES20
import android.util.Log
import android.view.Surface
import java.io.File

/** Отдельный app_process, без камеры/сети и перезапуска приложения.
 * CLASSPATH=probe.apk app_process -Djava.library.path=LIBDIR / com.utun.chat.data.CallVideoHevcProbe OUTPUT_DIR
 * LIBDIR содержит все arm64-v8a .so из того же APK, включая libonnxruntime.so; APK должен быть read-only.
 * Второй аргумент: parameters — vendor API 31+, call — production encode/bitrate, receive — *.key из OUTPUT_DIR,
 * capabilities — cold/warm probes, key-policy — работа без периодических KEY. Без аргумента исследует матрицу настроек.
 * Результаты: results.log, KEY и Annex B для внешнего decoder; камера/сеть не используются. */
object CallVideoHevcProbe {
    private const val TAG = "CallVideoHevcProbe"
    private const val FRAMES = 8
    private data class Settings(val surface: Boolean, val level: Boolean, val cbr: Boolean, val latency: Boolean = true) {
        val label get() = "${if (surface) "surface" else "yuv"}-${if (level) "level" else "auto"}-" +
            "${if (cbr) "cbr" else "default"}-${if (latency) "lowlatency" else "stream"}"
    }

    @JvmStatic fun main(args: Array<String>) {
        require(args.size in 1..2 && (args.size == 1 || args[1] in listOf("parameters", "call", "receive", "capabilities", "key-policy"))) {
            "Expected output directory [parameters|call|receive|capabilities|key-policy]"
        }
        check(Build.VERSION.SDK_INT >= 29) { "Hardware codec enumeration requires API 29" }
        @Suppress("DEPRECATION")
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper()
        val directory = File(args[0]); check(directory.isDirectory || directory.mkdirs())
        val journal = File(directory, "results.log")
        fun report(message: String) { Log.i(TAG, message); journal.appendText("$message\n") }
        if (args.getOrNull(1) == "key-policy") {
            val encoders = CallVideoCodec.probe(); val decoders = CallVideoCodec.probeDecoders()
            for (type in listOf(1, 2)) {
                val mode = checkNotNull(encoders[type]) { "No verified encoder for codec $type" }
                    .modes.minBy { it.size.width * it.size.height }
                val format = CallVideoFormat(1, mode.size.width, mode.size.height, fps = mode.fps, codec = type,
                    level = mode.level, profile = mode.profile)
                val decoderName = checkNotNull(decoders[type]?.modes?.firstOrNull { it.supports(format) }).name
                CallVideoCodec(format, 1_000_000, mode.name).use { codec ->
                    codec.prepare()
                    val syncAt = format.fps * 2 + 1
                    val base = 2_000_000L; val wanted = syncAt + FRAMES
                    val capturePts = List(wanted) { base + it * 1_000_000L / format.fps + if (it >= format.fps) 1_300_000L else 0L }
                    val packets = mutableListOf<CallVideoCodec.Packet>(); var input = 0
                    val deadline = System.nanoTime() + 5_000_000_000L
                    while (packets.size < wanted && System.nanoTime() < deadline) {
                        if (input < wanted) {
                            if (input == format.fps) check(codec.setBitrate(500_000))
                            if (codec.submit(null, capturePts[input], input == 0 || input == syncAt)) input++
                        }
                        packets += codec.drain(); Thread.sleep(2)
                    }
                    check(packets.size == wanted) { "Incomplete encode ${packets.size}/$wanted" }
                    check(packets.map { it.pts } == capturePts) { "Encoder changed capture PTS after pause" }
                    val keys = packets.indices.filter { packets[it].key }
                    check(keys.size == 2 && keys[0] == 0 && keys[1] >= syncAt) { "Unexpected KEY positions $keys requested at $syncAt" }
                    CallVideoCodec.verifyDecode(codec.format, packets, decoderName)
                    report("key-policy PASSED codec=$type encoder=${mode.name} frames=$wanted keys=$keys requested_at=$syncAt")
                }
            }
            return
        }
        if (args.getOrNull(1) == "capabilities") {
            // Отмена до/после прогрева не должна публиковать или очищать проверенные режимы.
            check(CallVideoCodec.probeDecoders { false }.isEmpty() && CallVideoCodec.probe { false }.isEmpty())
            val started = System.nanoTime()
            val decoders = CallVideoCodec.probeDecoders(); val receiveAt = System.nanoTime()
            check(decoders.isNotEmpty()) { "No verified video decoders" }
            report("capabilities cold receiveMs=${(receiveAt - started) / 1_000_000} modes=${decoders.values.sumOf { it.modes.size }}")
            val encoders = CallVideoCodec.probe(); val duplexAt = System.nanoTime()
            check(encoders.isNotEmpty()) { "No verified video encoders" }
            report("capabilities cold encodeMs=${(duplexAt - receiveAt) / 1_000_000} totalMs=${(duplexAt - started) / 1_000_000} " +
                "modes=${encoders.values.sumOf { it.modes.size }}")
            check(CallVideoCodec.probeDecoders { false }.isEmpty() && CallVideoCodec.probe { false }.isEmpty())
            val warmAt = System.nanoTime()
            repeat(3) { check(CallVideoCodec.probeDecoders() == decoders && CallVideoCodec.probe() == encoders) }
            report("capabilities PASSED repeatCalls=3 totalUs=${(System.nanoTime() - warmAt) / 1000} verifiedModesUnchanged=true")
            return
        }
        val encoder = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.first {
            it.isEncoder && it.isHardwareAccelerated && "video/hevc" in it.supportedTypes
        }
        val decoder = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.first {
            !it.isEncoder && it.isHardwareAccelerated && "video/hevc" in it.supportedTypes
        }
        report("start encoder=${encoder.name} decoder=${decoder.name} frames=$FRAMES")
        if (args.getOrNull(1) == "receive") {
            val decoders = checkNotNull(CallVideoCodec.probeDecoders()[2]) { "No verified HEVC decoder" }
            val files = directory.listFiles()!!.filter { it.extension == "key" }.sortedBy { it.name }
            check(files.isNotEmpty()) { "No remote KEY files in $directory" }
            for (file in files) {
                val bytes = file.readBytes(); val inspected = checkNotNull(CallVideoNative.inspect(2, bytes))
                val fps = Regex("-fps(30|50|60)-").find(file.name)?.groupValues?.get(1)?.toInt() ?: 30
                val format = CallVideoFormat(1, inspected[0].toInt(), inspected[1].toInt(), fps = fps, codec = 2,
                    level = inspected[2].toInt(), profile = inspected[3].toInt())
                val implementation = checkNotNull(decoders.modes.firstOrNull { it.supports(format) })
                CallVideoCodec.verifyDecode(format, List(FRAMES) { CallVideoCodec.Packet(1_000_000L + it * 1_000_000L / fps, true, bytes) },
                    implementation.name)
                report("remote PASSED file=${file.name} format=$format decoder=${implementation.name}")
            }
            report("remote finished files=${files.size}")
            return
        }
        if (args.getOrNull(1) == "call") {
            val encoders = CallVideoCodec.probe()
            val decoders = CallVideoCodec.probeDecoders()
            val modes = checkNotNull(encoders[2]) { "No verified HEVC encoder" }.modes
            val receiving = checkNotNull(decoders[2]) { "No verified HEVC decoder" }
            report("production encoders=$modes decoders=$receiving")
            for (mode in modes) for (portrait in listOf(false, true)) {
                val format = CallVideoFormat(1, if (portrait) mode.size.height else mode.size.width,
                    if (portrait) mode.size.width else mode.size.height, fps = mode.fps, codec = 2, level = mode.level, profile = mode.profile)
                val implementation = checkNotNull(receiving.modes.firstOrNull { it.supports(format) })
                CallVideoCodec(format, 1_000_000, mode.name).use { codec ->
                    codec.prepare()
                    check(codec.format == format) { "Production mode differs from probe: ${codec.format}/$format" }
                    for ((phase, rate) in listOf(1_000_000, 4_000_000, 300_000, 4_000_000).withIndex()) {
                        check(codec.setBitrate(rate)) { "Bitrate update refused $rate" }
                        val packets = mutableListOf<CallVideoCodec.Packet>(); var input = 0
                        val base = 2_000_000L + phase * 1_000_000L
                        val deadline = System.nanoTime() + 5_000_000_000L
                        while (packets.size < FRAMES && System.nanoTime() < deadline) {
                            if (input < FRAMES && codec.submit(null, base + input * 1_000_000L / format.fps, input == 0)) ++input
                            packets += codec.drain(); Thread.sleep(2)
                        }
                        check(packets.size == FRAMES && packets.first().key) { "Incomplete production encode" }
                        CallVideoCodec.verifyDecode(codec.format, packets, implementation.name)
                        report("production PASSED format=${codec.format} rate=$rate frames=${packets.size} decoder=${implementation.name}")
                        File(directory, "production-${format.width}x${format.height}-fps${format.fps}-$phase.key").writeBytes(packets.first().bytes)
                    }
                }
            }
            report("production finished modes=${modes.size}")
            return
        }
        if (args.getOrNull(1) == "parameters") {
            check(Build.VERSION.SDK_INT >= 31) { "Vendor parameter enumeration requires API 31" }
            val codec = MediaCodec.createByCodecName(encoder.name)
            try {
                for (parameter in codec.supportedVendorParameters) report("vendor parameter=$parameter type=${codec.getParameterDescriptor(parameter)?.type}")
            } finally { codec.release() }
            return
        }
        val variants = listOf(false, true).flatMap { surface -> listOf(true, false).flatMap { level ->
            listOf(true, false).map { cbr -> Settings(surface, level, cbr) }
        } } + Settings(true, false, false, false)
        for ((width, height) in listOf(640 to 480, 480 to 640, 1280 to 720, 720 to 1280, 1920 to 1080, 1080 to 1920)) {
            for (settings in variants) {
                val label = "${width}x$height-${settings.label}"
                try {
                    val packets = encode(encoder.name, width, height, settings) { report("$label $it") }
                    val first = packets.first(); check(first.key) { "First output is not KEY" }
                    val key = first.bytes
                    File(directory, "$label.key").writeBytes(key)
                    File(directory, "$label.h265").outputStream().use { out ->
                        for (packet in packets) {
                            if (packet.key) StreamVideoCodec.H265.unpackKey(packet.bytes).let { out.write(it.first); out.write(it.second) }
                            else out.write(packet.bytes)
                        }
                    }
                    val config = StreamVideoCodec.H265.unpackKey(key).first
                    report("$label config=${config.joinToString("") { "%02x".format(it.toInt() and 255) }}")
                    val inspected = CallVideoNative.inspect(2, key)
                    report("$label validator=${inspected?.contentToString() ?: "REJECTED"} encoded=${packets.size}")
                    val format = CallVideoFormat(1, width, height, codec = 2, level = if (width * height > 1280 * 720) 120 else 93, profile = 1)
                    val pipeline = CallVideoCodec.verifyDecode(format, packets, decoder.name)
                    report("$label DECODED frames=${packets.size} pipeline=$pipeline")
                } catch (e: Exception) {
                    Log.e(TAG, "$label failed", e); report("$label FAILED ${e.javaClass.simpleName}: ${e.message}")
                }
            }
        }
        report("finished encoder=${encoder.name}")
    }

    private fun encode(name: String, width: Int, height: Int, settings: Settings, report: (String) -> Unit): List<CallVideoCodec.Packet> {
        val codec = MediaCodec.createByCodecName(name)
        var surface: Surface? = null
        var display = EGL14.EGL_NO_DISPLAY
        var context = EGL14.EGL_NO_CONTEXT
        var window = EGL14.EGL_NO_SURFACE
        var started = false
        var problem: Throwable? = null
        try {
            if (settings.surface) display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            val format = MediaFormat.createVideoFormat("video/hevc", width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, if (settings.surface) MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface
                    else MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
                setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain)
                if (settings.level) setInteger(MediaFormat.KEY_LEVEL, CallVideoCodec.platformLevel(2, if (width * height > 1280 * 720) 120 else 93))
                if (settings.cbr) setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
                setInteger(MediaFormat.KEY_BIT_RATE, 1_000_000); setInteger(MediaFormat.KEY_FRAME_RATE, 30)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1); setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
                if (settings.latency) setInteger(MediaFormat.KEY_LATENCY, 0)
            }
            report("requested=$format")
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            if (settings.surface) {
                surface = codec.createInputSurface()
                val version = IntArray(2); check(EGL14.eglInitialize(display, version, 0, version, 1)) { "EGL initialize" }
                val configs = arrayOfNulls<android.opengl.EGLConfig>(1); val count = IntArray(1)
                val attributes = intArrayOf(EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT, EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    0x3142, 1, EGL14.EGL_NONE) // EGL_RECORDABLE_ANDROID
                check(EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) && count[0] > 0) { "EGL choose config" }
                context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                    intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
                check(context != EGL14.EGL_NO_CONTEXT) { "EGL create context" }
                window = EGL14.eglCreateWindowSurface(display, configs[0], surface, intArrayOf(EGL14.EGL_NONE), 0)
                check(window != EGL14.EGL_NO_SURFACE && EGL14.eglMakeCurrent(display, window, window, context)) { "EGL create window" }
            }
            codec.start(); started = true
            val packets = mutableListOf<CallVideoCodec.Packet>(); val info = MediaCodec.BufferInfo()
            val partial = java.io.ByteArrayOutputStream(); var partialPts = -1L; var partialFlags = 0
            var config = byteArrayOf(); var sent = 0
            val start = System.nanoTime(); val deadline = start + 5_000_000_000L
            while (packets.size < FRAMES && System.nanoTime() < deadline) {
                if (sent < FRAMES && System.nanoTime() - start >= sent * 33_333_000L) {
                    val pts = 1_000_000L + sent * 33333
                    if (settings.surface) {
                        GLES20.glClearColor(0.2f + sent * 0.02f, 0.3f, 0.4f, 1f); GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
                        check(GLES20.glGetError() == GLES20.GL_NO_ERROR) { "GL clear" }
                        check(EGLExt.eglPresentationTimeANDROID(display, window, pts * 1000)) { "EGL timestamp" }
                        check(EGL14.eglSwapBuffers(display, window)) { "EGL swap" }; sent++
                    } else {
                        val input = codec.dequeueInputBuffer(0)
                        if (input >= 0) {
                            checkNotNull(codec.getInputImage(input)).use { image ->
                                for ((planeIndex, plane) in image.planes.withIndex()) {
                                    val w = width shr (if (planeIndex == 0) 0 else 1)
                                    val h = height shr (if (planeIndex == 0) 0 else 1)
                                    val row = ByteArray((w - 1) * plane.pixelStride + 1) { if (planeIndex == 0) (64 + sent).toByte() else 128.toByte() }
                                    val buffer = plane.buffer.duplicate(); val offset = buffer.position()
                                    for (y in 0 until h) { buffer.position(offset + y * plane.rowStride); buffer.put(row) }
                                }
                            }
                            codec.queueInputBuffer(input, 0, width * height * 3 / 2, pts, 0); sent++
                        }
                    }
                }
                val index = codec.dequeueOutputBuffer(info, 1000)
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    val actual = codec.outputFormat
                    report("output=$actual")
                    actual.getByteBuffer("csd-0")?.duplicate()?.let { b -> config = ByteArray(b.remaining()).also(b::get) }
                } else if (index >= 0) {
                    try {
                        if (info.size > 0) {
                            val buffer = checkNotNull(codec.getOutputBuffer(index)).duplicate()
                            buffer.position(info.offset); buffer.limit(info.offset + info.size)
                            val bytes = ByteArray(info.size).also(buffer::get)
                            if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) config = StreamVideoCodec.H265.mergeConfig(config, bytes)
                            else {
                                if (partial.size() == 0) { partialPts = info.presentationTimeUs; partialFlags = 0 }
                                check(partialPts == info.presentationTimeUs && partial.size() + bytes.size <= H264AnnexB.MAX_FRAME)
                                partial.write(bytes); partialFlags = partialFlags or info.flags
                                if (info.flags and MediaCodec.BUFFER_FLAG_PARTIAL_FRAME == 0) {
                                    val key = partialFlags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                                    val data = partial.toByteArray(); partial.reset()
                                    packets += CallVideoCodec.Packet(info.presentationTimeUs, key,
                                        if (key) StreamVideoCodec.H265.keyFrame(data, config) else StreamVideoCodec.H265.normalize(data))
                                }
                            }
                        }
                    } finally { codec.releaseOutputBuffer(index, false) }
                }
            }
            check(sent == FRAMES && packets.size == FRAMES) { "Incomplete encode input=$sent output=${packets.size}" }
            for ((i, packet) in packets.withIndex()) check(packet.pts == 1_000_000L + i * 33333) { "Reordered encoder PTS" }
            return packets
        } catch (e: Throwable) {
            problem = e
            throw e
        } finally {
            var cleanupFailure: Exception? = null
            fun cleanup(stage: String, action: () -> Unit) {
                try { action() } catch (e: Exception) {
                    Log.e(TAG, "cleanup failed stage=$stage codec=$name", e)
                    if (problem != null) problem!!.addSuppressed(e)
                    else if (cleanupFailure == null) cleanupFailure = e else cleanupFailure!!.addSuppressed(e)
                }
            }
            if (display != EGL14.EGL_NO_DISPLAY) {
                cleanup("EGL unbind") {
                    check(EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT))
                }
                if (window != EGL14.EGL_NO_SURFACE) cleanup("EGL surface") { check(EGL14.eglDestroySurface(display, window)) }
                if (context != EGL14.EGL_NO_CONTEXT) cleanup("EGL context") { check(EGL14.eglDestroyContext(display, context)) }
                cleanup("EGL thread") { check(EGL14.eglReleaseThread()) }
                cleanup("EGL display") { check(EGL14.eglTerminate(display)) }
            }
            if (started) cleanup("codec stop") { codec.stop() }
            cleanup("codec release") { codec.release() }
            cleanup("input Surface") { surface?.release() }
            cleanupFailure?.let { throw it }
        }
    }
}
