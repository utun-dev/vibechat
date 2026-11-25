package com.utun.chat.ui.screens

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size

/** UI-состояние файла/live: scale=1..4, offset — px относительно центра viewport.
 * transform получает конечные centroid/pan в px и положительный factor; video/viewport — конечные неотрицательные размеры.
 * Масштаб сохраняет точку под пальцами; constrain повторно ограничивает края при изменении размеров/поворота.
 */
internal class VideoZoomState {
    var scale by mutableStateOf(1f)
        private set
    var offset by mutableStateOf(Offset.Zero)
        private set

    fun transform(centroid: Offset, pan: Offset, factor: Float, video: Size, viewport: Size) {
        val next = (scale * factor).coerceIn(1f, 4f)
        val ratio = next / scale
        val relative = centroid - Offset(viewport.width / 2f, viewport.height / 2f)
        offset = offset * ratio + relative * (1f - ratio) + pan
        scale = next
        constrain(video, viewport)
    }

    fun constrain(video: Size, viewport: Size) {
        val x = ((video.width * scale - viewport.width) / 2f).coerceAtLeast(0f)
        val y = ((video.height * scale - viewport.height) / 2f).coerceAtLeast(0f)
        offset = Offset(offset.x.coerceIn(-x, x), offset.y.coerceIn(-y, y))
    }
}
