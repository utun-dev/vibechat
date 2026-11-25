package com.utun.chat.data

/** AUTO следует датчику, остальные углы задаются относительно обычного положения устройства.
 * deviceDegrees/sensor — четверти оборота 0/90/180/270; targetDegrees даёт ориентацию Surface,
 * sensorRotation добавляет положение сенсора и направление front. На wire AUTO=-1 не передаётся. */
enum class StreamOrientation(val degrees: Int) {
    // -1 хранится только в настройках; в видео передаётся вычисленный угол 0/90/180/270.
    AUTO(-1), UPRIGHT(0), RIGHT(90), UPSIDE_DOWN(180), LEFT(270);

    /** Целевой поворот CameraX/Surface; датчик положения отсчитывает угол в противоположную сторону. */
    fun targetDegrees(deviceDegrees: Int = 0): Int = if (this == AUTO) (360 - deviceDegrees) % 360 else degrees

    fun sensorRotation(sensor: Int, front: Boolean, deviceDegrees: Int = 0): Int {
        val turn = targetDegrees(deviceDegrees)
        return (sensor + (if (front) turn else -turn) + 360) % 360
    }
}

/** Подтверждаемый режим камеры/ориентации: videoBitrate=null использует автоматически рассчитанный mode.bitrate.
 * Битрейт в бит/с проверяется по аппаратному диапазону; withMode сохраняет ориентацию и ограничивает
 * ручной выбор новым manualBitrateRange (до 12 Мбит/с), либо снимает ручной выбор при пустом диапазоне. */
data class StreamCaptureConfig(val mode: StreamCaptureMode, val orientation: StreamOrientation = StreamOrientation.AUTO,
                               val videoBitrate: Int? = null) {
    val bitrate get() = videoBitrate ?: mode.bitrate
    init { require(bitrate in mode.minimumBitrate..mode.maximumBitrate) }
    fun withMode(next: StreamCaptureMode) = copy(mode = next,
        videoBitrate = videoBitrate?.takeUnless { next.manualBitrateRange.isEmpty() }?.coerceIn(next.manualBitrateRange))
}
