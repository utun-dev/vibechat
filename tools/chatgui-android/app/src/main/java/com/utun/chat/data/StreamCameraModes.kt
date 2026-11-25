package com.utun.chat.data

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.os.Build

/** Кандидат режима камеры/Surface encoder: codec — аппаратное имя, videoCodec — wire ID/MIME.
 * minimumFps/fps задают AE range, highSpeed требует constrained session; bitrate/границы — бит/с.
 * Наличие capability ещё требует реального запуска encoder/camera владельцем захвата. */
data class StreamCaptureMode(val camera: String, val width: Int, val height: Int, val fps: Int,
                             val minimumFps: Int, val highSpeed: Boolean, val codec: String, val bitrate: Int,
                             val videoCodec: StreamVideoCodec = StreamVideoCodec.H264,
                             val minimumBitrate: Int = 1, val maximumBitrate: Int = Int.MAX_VALUE,
                             val encoderProfile: Int? = videoCodec.profile, val screen: Boolean = false) {
    val label get() = "${width}×${height} · $fps fps"
    val manualBitrateRange get() = minimumBitrate..minOf(maximumBitrate, 12_000_000)
}
data class StreamCamera(val id: String, val lens: LensOption, val modes: List<StreamCaptureMode>)

/** Capability enumeration без открытия camera/codec: обычные <=1080p, high speed 120/240 при поддержке.
 * Ошибка одного encoder/камеры логируется и пропускается; ошибка получения общего списка API передаётся caller. */
object StreamCameraModes {
    fun enumerate(context: Context): List<StreamCamera> {
        val manager = context.getSystemService(CameraManager::class.java)
        val codecs = encoders()
        return CameraLensManager.enumerate(manager).mapNotNull { lens ->
            try {
                val c = manager.getCameraCharacteristics(lens.cameraId)
                val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: error("Missing stream map")
                val output = map.getOutputSizes(android.media.MediaCodec::class.java).orEmpty().toList()
                val common = output.filter { it.width <= 1920 && it.height <= 1080 &&
                    (it.width to it.height) in setOf(640 to 480, 1280 to 720, 1920 to 1080) }
                val sizes = common.ifEmpty { output.filter { it.width <= 1280 && it.height <= 720 }
                    .maxByOrNull { it.width * it.height }?.let { listOf(it) }.orEmpty() }
                val modes = mutableListOf<StreamCaptureMode>()
                val rates = c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES).orEmpty()
                val normalFps = listOf(30, 60).filter { fps -> rates.any { it.upper == fps } }
                    .ifEmpty { rates.map { it.upper }.filter { it <= 30 }.maxOrNull()?.let { listOf(it) }.orEmpty() }
                for (size in sizes) for (fps in normalFps) {
                    val duration = map.getOutputMinFrameDuration(android.media.MediaCodec::class.java, size)
                    if (duration > 0 && duration > 1_000_000_000L / fps + 1) continue
                    val range = rates.filter { it.upper == fps }.maxByOrNull { it.lower } ?: continue
                    add(codecs, modes, lens.cameraId, false, size.width, size.height, fps, range.lower, false)
                }
                if ((c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf())
                    .contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO)) {
                    for (size in map.highSpeedVideoSizes.orEmpty()) {
                        if (size !in output || size.width > 1920 || size.height > 1080) continue
                        for (rate in map.getHighSpeedVideoFpsRangesFor(size)) {
                            if (rate.lower == rate.upper && rate.upper in listOf(120, 240))
                                add(codecs, modes, lens.cameraId, false, size.width, size.height, rate.upper, rate.lower, true)
                        }
                    }
                }
                if (modes.isEmpty()) { LogManager.addLog("WARN", "video", "camera has no compatible live modes id=${lens.cameraId}"); null }
                else StreamCamera(lens.cameraId, lens, modes.distinct().sortedWith(compareBy({ it.videoCodec.id }, { it.fps }, { it.width * it.height })))
            } catch (e: Exception) { LogManager.addLog("ERROR", "video", "camera mode enumeration id=${lens.cameraId}: ${e.message}"); null }
        }.sortedBy { when (it.lens.type) { LensType.MAIN -> 0; LensType.SELFIE -> 1; else -> 2 } }
    }

    private fun add(codecs: List<MediaCodecInfo>, modes: MutableList<StreamCaptureMode>, cameraId: String,
                    screen: Boolean, width: Int, height: Int, fps: Int, lower: Int, highSpeed: Boolean) {
        for (type in StreamVideoCodec.values()) {
            for (codec in codecs) {
                try {
                    if (codec.supportedTypes.none { it.equals(type.mime, true) }) continue
                    if (highSpeed && Build.VERSION.SDK_INT >= 29 && !codec.isHardwareAccelerated) continue
                    val caps = codec.getCapabilitiesForType(type.mime)
                    val video = caps.videoCapabilities ?: continue
                    val profile = type.encoderProfile(caps.profileLevels.map { it.profile }.toIntArray())
                    if (!caps.colorFormats.contains(MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface) ||
                        !video.areSizeAndRateSupported(width, height, fps.toDouble())) continue
                    val bitrate = (width.toLong() * height * fps / 14).coerceIn(512_000L, 16_000_000L).toInt()
                    modes.add(StreamCaptureMode(cameraId, width, height, fps, lower, highSpeed, codec.name,
                        video.bitrateRange.clamp(bitrate), type, video.bitrateRange.lower, video.bitrateRange.upper, profile, screen))
                    LogManager.addLog("INFO", "video", "capture mode camera=$cameraId screen=$screen ${width}x$height fps=$lower..$fps " +
                        "highSpeed=$highSpeed type=${type.label} encoder=${codec.name} profile=$profile bitrate=${modes.last().bitrate} " +
                        "range=${video.bitrateRange} achievable=${video.getAchievableFrameRatesFor(width, height)}")
                    break
                } catch (e: Exception) { LogManager.addLog("WARN", "video", "codec probe ${codec.name}: ${e.message}") }
            }
        }
    }

    private fun encoders() = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { it.isEncoder }
        .sortedBy { if (Build.VERSION.SDK_INT >= 29 && it.isHardwareAccelerated) 0 else 1 }

    fun screenModes(portrait: Boolean): List<StreamCaptureMode> {
        val modes = mutableListOf<StreamCaptureMode>()
        val codecs = encoders()
        for ((long, short) in listOf(1280 to 720, 1920 to 1080, 640 to 480)) {
            add(codecs, modes, "", true, if (portrait) short else long, if (portrait) long else short, 30, 30, false)
        }
        return modes
    }

    /** Сохранить codec, если он доступен; затем размер/FPS либо тот же размер с меньшей FPS.
     * Fallback: 720p30, затем обычный режим с максимумом FPS/площади, затем первый; пустой список даёт null. */
    fun choose(modes: List<StreamCaptureMode>, previous: StreamCaptureMode? = null): StreamCaptureMode? {
        val preferred = modes.filter { it.videoCodec == (previous?.videoCodec ?: StreamVideoCodec.H264) }
        if (preferred.isNotEmpty() && preferred.size != modes.size) return choose(preferred, previous)
        if (previous != null) {
            modes.firstOrNull { it.width == previous.width && it.height == previous.height && it.fps == previous.fps }?.let { return it }
            modes.filter { it.width == previous.width && it.height == previous.height && it.fps <= previous.fps }
                .maxByOrNull { it.fps }?.let { return it }
        }
        return modes.firstOrNull { it.width == 1280 && it.height == 720 && it.fps == 30 }
            ?: modes.filter { !it.highSpeed }.maxWithOrNull(compareBy({ it.fps }, { it.width * it.height })) ?: modes.firstOrNull()
    }
}
