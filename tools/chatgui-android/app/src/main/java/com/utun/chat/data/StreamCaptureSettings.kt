package com.utun.chat.data

/** Сохранённый выбор без аппаратных capabilities; resolve сопоставляет его с новым списком камер/modes.
 * Камера по ID, затем MAIN/первая; codec сохраняется, если доступен; размер/FPS — точный либо меньшая FPS.
 * Дальнейший fallback выбирает StreamCameraModes, неверная orientation становится AUTO, bitrate ограничивается.
 * Пустой набор камеры/modes возвращает null с WARN; фактическое восстановление логируется, audio хранится отдельно. */
data class StreamCaptureSettings(val camera: String = "", val width: Int = 1280, val height: Int = 720, val fps: Int = 30,
                                 val codec: Int = StreamVideoCodec.H264.id, val rotation: Int = StreamOrientation.AUTO.degrees,
                                 val bitrate: Int? = null, val audio: Boolean = true) {
    fun resolve(cameras: List<StreamCamera>): StreamCaptureConfig? {
        val selected = cameras.firstOrNull { it.id == camera }
            ?: cameras.firstOrNull { it.lens.type == LensType.MAIN } ?: cameras.firstOrNull()
        if (selected == null) {
            LogManager.addLog("WARN", "stream", "capture settings: no available camera"); return null
        }
        val modes = selected.modes.filter { it.videoCodec.id == codec }.ifEmpty { selected.modes }
        val mode = modes.firstOrNull { it.width == width && it.height == height && it.fps == fps }
            ?: modes.filter { it.width == width && it.height == height && it.fps <= fps }.maxByOrNull { it.fps }
            ?: StreamCameraModes.choose(modes)
        if (mode == null) {
            LogManager.addLog("WARN", "stream", "capture settings: camera=${selected.id} has no available mode"); return null
        }
        val orientation = StreamOrientation.values().firstOrNull { it.degrees == rotation } ?: StreamOrientation.AUTO
        val restored = StreamCaptureConfig(mode, orientation,
            bitrate?.takeUnless { mode.manualBitrateRange.isEmpty() }?.coerceIn(mode.manualBitrateRange))
        if (camera.isNotEmpty() && (camera != mode.camera || width != mode.width || height != mode.height || fps != mode.fps ||
                codec != mode.videoCodec.id || rotation != orientation.degrees || bitrate != restored.videoBitrate))
            LogManager.addLog("WARN", "stream", "capture settings adjusted: saved=$this restored=$restored")
        LogManager.addLog("INFO", "stream", "capture settings restored camera=${mode.camera} mode=${mode.label} " +
            "codec=${mode.videoCodec.label} rotation=${orientation.degrees} bitrate=${restored.videoBitrate ?: "auto"} audio=$audio")
        return restored
    }
}
