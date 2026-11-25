package com.utun.chat.ui.components

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChanged
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity

private const val MAX_ZOOM = 4f

private fun clampOffset(scale: Float, o: Offset, cW: Float, cH: Float, iw: Float, ih: Float): Offset {
    val fit = minOf(cW / iw, cH / ih)
    val dw = iw * fit * scale
    val dh = ih * fit * scale
    val mx = ((dw - cW) / 2f).coerceAtLeast(0f)
    val my = ((dh - cH) / 2f).coerceAtLeast(0f)
    return Offset(o.x.coerceIn(-mx, mx), o.y.coerceIn(-my, my))
}

/** Целый bitmap в ограниченном viewport с ContentScale.Fit: zoom=1..4, pan удерживает края изображения.
 * Bitmap принадлежит caller и должен оставаться живым до dispose. Новый bitmap сбрасывает zoom/pan.
 * На масштабе 1 одиночный горизонтальный свайп >96dp вызывает onSwipe(+1 влево, -1 вправо) один раз за gesture.
 */
@Composable
fun ZoomableImage(bitmap: Bitmap, modifier: Modifier = Modifier, onSwipe: ((Int) -> Unit)? = null) {
    var zoomScale by remember(bitmap) { mutableStateOf(1f) }
    var zoomOffset by remember(bitmap) { mutableStateOf(Offset.Zero) }
    val navigate by rememberUpdatedState(onSwipe)

    BoxWithConstraints(modifier) {
        val density = LocalDensity.current
        val cW = with(density) { maxWidth.toPx() }
        val cH = with(density) { maxHeight.toPx() }
        val iw = bitmap.width.toFloat()
        val ih = bitmap.height.toFloat()

        LaunchedEffect(cW, cH) {
            zoomOffset = clampOffset(zoomScale, zoomOffset, cW, cH, iw, ih)
        }

        Image(
            bitmap.asImageBitmap(),
            contentDescription = null,
            contentScale = ContentScale.Fit,
            modifier = Modifier
                .fillMaxSize()
                .graphicsLayer {
                    scaleX = zoomScale
                    scaleY = zoomScale
                    translationX = zoomOffset.x
                    translationY = zoomOffset.y
                }
        )

        Box(
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(bitmap,cW,cH) {
                    awaitEachGesture {
                        awaitFirstDown(requireUnconsumed=false)
                        var swipe=0f; var swiped=false; var canSwipe=zoomScale==1f
                        do {
                        val event=awaitPointerEvent()
                        if (event.changes.none { it.pressed }) break
                        val centroid=event.calculateCentroid(useCurrent=false)
                        if (!centroid.x.isFinite() || !centroid.y.isFinite()) continue
                        val pan=event.calculatePan(); val zoom=event.calculateZoom()
                        if (event.changes.count { it.pressed }!=1 || zoom!=1f) canSwipe=false
                        if (canSwipe && !swiped && navigate!=null && kotlin.math.abs(pan.x)>kotlin.math.abs(pan.y)) {
                            swipe+=pan.x
                            if (kotlin.math.abs(swipe)>96*density.density) { navigate?.invoke(if (swipe<0) 1 else -1); swiped=true }
                        }
                        val old = zoomScale
                        val new = (zoomScale * zoom).coerceIn(1f, MAX_ZOOM)
                        val z = new / old
                        val rel = centroid - Offset(cW / 2f, cH / 2f)
                        val raw = Offset(
                            zoomOffset.x * z + rel.x * (1f - z) + pan.x,
                            zoomOffset.y * z + rel.y * (1f - z) + pan.y
                        )
                        zoomScale = new
                        zoomOffset = clampOffset(new, raw, cW, cH, iw, ih)
                        if (zoom!=1f || pan!=Offset.Zero) event.changes.forEach { if (it.positionChanged()) it.consume() }
                        } while (event.changes.any { it.pressed })
                    }
                }
        )
    }
}
