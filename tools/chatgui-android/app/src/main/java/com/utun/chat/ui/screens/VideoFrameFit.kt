package com.utun.chat.ui.screens

/** Размер слоя ДО rotation; поворот относится только к видео, не к окну/кнопкам. */
internal data class VideoFrameFit(val width: Float, val height: Float, val rotation: Float)

/** Вписать целый кадр без растяжения: viewport width/height — конечные неотрицательные px, aspect=width/height.
 * Неверный aspect заменяется квадратом; allowTurn выбирает 0/90° по максимальной площади, при равенстве — 0°.
 */
internal fun fitVideoFrame(width: Float, height: Float, aspect: Float, allowTurn: Boolean): VideoFrameFit {
    val ratio = aspect.takeIf { it.isFinite() && it > 0f } ?: 1f
    val normal = minOf(width, height * ratio)
    val rotated = minOf(height, width * ratio)
    val turn = allowTurn && rotated > normal
    val fittedWidth = if (turn) rotated else normal
    return VideoFrameFit(fittedWidth, fittedWidth / ratio, if (turn) 90f else 0f)
}
