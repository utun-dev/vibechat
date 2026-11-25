package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ActivityInfo
import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.ScreenRotation
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.utun.chat.data.LogManager
import com.utun.chat.data.VideoFilePlayer
import com.utun.chat.data.VideoFileMetadata
import com.utun.chat.data.VideoInfo
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

/** Локальное видео: MediaPlayer принадлежит VideoFilePlayer, metadata читаются отдельно в IO.
 * Controls показывают подтверждённое SDK состояние; zoom/pan и пауза сохраняются при смене размеров Surface.
 * Завершение файла закрывает экран. Ориентация и панели окна восстанавливаются при выходе. */
@Composable
fun VideoPlayerScreen(filePath: String, onClose: () -> Unit, onOpened: (String) -> Unit = {}) {
    val completed by rememberUpdatedState(onClose)
    val player = remember(filePath) { VideoFilePlayer { completed() } }
    val playback by player.state.collectAsState()
    val error = playback.error
    val isPlaying = playback.playing
    val durationMs = playback.durationMs
    val positionMs = playback.positionMs
    var reported by remember(filePath) { mutableStateOf(false) }
    LaunchedEffect(filePath, playback.prepared, error) {
        if (playback.prepared && error == null && !reported) { reported = true; onOpened(filePath) }
    }
    var controlsVisible by remember(filePath) { mutableStateOf(true) }
    var dragging by remember(filePath) { mutableStateOf(false) }
    var dragPosition by remember(filePath) { mutableStateOf(0) }
    var wasPlayingBeforeDrag by remember(filePath) { mutableStateOf(false) }
    var videoInfo by remember(filePath) { mutableStateOf<VideoInfo?>(null) }
    val zoom = remember(filePath) { VideoZoomState() }
    var hideTick by remember { mutableStateOf(0) }

    val context = LocalContext.current

    BackHandler(onBack = onClose)
    ViewerFullscreenWindow("video")

    /* ── video metadata (dimensions + rotation) ── */
    LaunchedEffect(filePath) {
        val info = try { withContext(Dispatchers.IO) { VideoFileMetadata().use { it.read(filePath) } } }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { LogManager.addLog("WARN", "VideoPlayer", "metadata failed: $e"); null }
        videoInfo = info
        if (info != null) {
            LogManager.addLog(
                "INFO", "VideoPlayer",
                "video ${info.width}x${info.height} rot=${info.rotation} -> display ${info.displayWidth}x${info.displayHeight} aspect=%.3f".format(info.aspect)
            )
        }
    }

    /* ── auto-rotate screen to match video orientation ── */
    DisposableEffect(context, videoInfo) {
        val act = context.findActivity()
        val prev = act?.requestedOrientation
        val info = videoInfo
        if (act != null && info != null && info.displayWidth > 0 && info.displayHeight > 0) {
            val orientation = when {
                info.displayWidth > info.displayHeight -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                info.displayHeight > info.displayWidth -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
                else -> prev
            }
            if (orientation != null) {
                try {
                    act.requestedOrientation = orientation
                    LogManager.addLog("INFO", "VideoPlayer", "Fit screen video=${info.displayWidth}x${info.displayHeight} " +
                        "metadataRotation=${info.rotation} requested=$orientation previous=$prev")
                } catch (e: Exception) { LogManager.addLog("ERROR", "VideoPlayer", "Cannot apply orientation=$orientation: $e") }
            }
        }
        onDispose {
            if (act != null && prev != null) {
                try {
                    act.requestedOrientation = prev
                    LogManager.addLog("INFO", "VideoPlayer", "restore orientation $prev")
                } catch (e: Exception) { LogManager.addLog("ERROR", "VideoPlayer", "Cannot restore orientation=$prev: $e") }
            }
        }
    }

    /* ── MediaPlayer lifecycle ── */
    DisposableEffect(player) {
        player.start(filePath)
        onDispose { player.close() }
    }

    /* ── position polling ── */
    LaunchedEffect(player, isPlaying) {
        while (isPlaying && isActive) {
            player.pollPosition()
            delay(300)
        }
    }

    /* ── controls auto-hide (only while playing) ── */
    LaunchedEffect(controlsVisible, isPlaying, hideTick) {
        if (controlsVisible && isPlaying) {
            delay(3000)
            controlsVisible = false
        }
    }

    /* ── controls always visible while paused ── */
    LaunchedEffect(isPlaying) {
        if (!isPlaying) {
            controlsVisible = true
        }
    }

    fun onInteraction() {
        hideTick++
        controlsVisible = true
    }

    fun togglePlay() {
        if (error != null || !playback.prepared) return
        onInteraction()
        if (isPlaying) {
            player.pause()
        } else {
            if (durationMs > 0 && positionMs >= durationMs - 50) {
                if (!player.seek(0)) return
            }
            player.play()
        }
    }

    fun flipLandscape() {
        onInteraction()
        val act = context.findActivity() ?: return
        try {
            val rotation = act.windowManager.defaultDisplay.rotation
            val reverse = rotation == Surface.ROTATION_180 || rotation == Surface.ROTATION_270
            val newOrient = if (reverse) ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE else ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
            act.requestedOrientation = newOrient
            LogManager.addLog("INFO", "VideoPlayer", "flip 180 -> $newOrient")
        } catch (e: Exception) { LogManager.addLog("ERROR", "VideoPlayer", "Cannot flip screen orientation: $e") }
    }

    val isLandscape = (videoInfo?.displayWidth ?: 0) > (videoInfo?.displayHeight ?: 0)

    BoxWithConstraints(
        modifier = Modifier.fillMaxSize().background(Color.Black)
    ) {
        val ratio = videoInfo?.aspect ?: 0f
        val w: Dp
        val h: Dp
        if (ratio > 0f) {
            if (maxWidth / ratio <= maxHeight) {
                w = maxWidth
                h = maxWidth / ratio
            } else {
                h = maxHeight
                w = maxHeight * ratio
            }
        } else {
            w = maxWidth
            h = maxHeight
        }
        val videoModifier = if (ratio > 0f) {
            Modifier.size(w, h).align(Alignment.Center)
        } else {
            Modifier.fillMaxSize()
        }
        val density = LocalDensity.current
        val screenW = with(density) { maxWidth.toPx() }
        val screenH = with(density) { maxHeight.toPx() }
        val videoW = with(density) { w.toPx() }
        val videoH = with(density) { h.toPx() }

        /* re-clamp zoom offset when screen size changes (e.g. flip 180) */
        LaunchedEffect(screenW, screenH, videoW, videoH) {
            zoom.constrain(Size(videoW, videoH), Size(screenW, screenH))
        }

        key(player) { AndroidView(
            factory = { ctx ->
                TextureView(ctx).apply {
                    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                        override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int) {
                            player.attach(surface)
                        }
                        override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) {}
                        override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
                            return player.detach(surface)
                        }
                        override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {}
                    }
                }
            },
            update = { it.keepScreenOn = isPlaying }, onRelease = { player.close() },
            modifier = videoModifier.graphicsLayer {
                scaleX = zoom.scale
                scaleY = zoom.scale
                translationX = zoom.offset.x
                translationY = zoom.offset.y
            }
        ) }

        if (error != null) {
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                Text(stringResource(error), color = Color.White, fontSize = 16.sp)
            }
        }

        /* gestures: tap toggles controls; pinch zooms; drag pans */
        if (error == null) {
            Box(
                modifier = Modifier.fillMaxSize()
                    .pointerInput(player) {
                        detectTapGestures {
                            hideTick++
                            if (player.state.value.playing) controlsVisible = !controlsVisible
                        }
                    }
                    .pointerInput(player, screenW, screenH, videoW, videoH) {
                        detectTransformGestures { centroid, pan, factor, _ ->
                            onInteraction()
                            zoom.transform(centroid, pan, factor, Size(videoW, videoH), Size(screenW, screenH))
                        }
                    }
            )
        }

        AnimatedVisibility(
            visible = controlsVisible,
            enter = fadeIn(tween(1000)),
            exit = fadeOut(tween(1000)),
            modifier = Modifier.fillMaxSize()
        ) {
            Box(modifier = Modifier.fillMaxSize()) {
                /* top bar: close (right) */
                Row(
                    modifier = Modifier.fillMaxWidth()
                        .background(Color(0xAA000000))
                        .statusBarsPadding()
                        .padding(horizontal = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.End
                ) {
                    IconButton(onClick = onClose) {
                        Icon(Icons.Filled.Close, stringResource(R.string.close), tint = Color.White)
                    }
                }

                /* center: play/pause */
                Box(
                    modifier = Modifier.fillMaxSize(),
                    contentAlignment = Alignment.Center
                ) {
                    if (error == null) {
                        IconButton(
                            onClick = { togglePlay() },
                            enabled = playback.prepared,
                            modifier = Modifier.size(72.dp).background(Color(0xCC000000), MaterialTheme.shapes.extraLarge)
                        ) {
                            Icon(
                                if (isPlaying) Icons.Filled.Pause else Icons.Filled.PlayArrow,
                                contentDescription = stringResource(if (isPlaying) R.string.pause else R.string.play),
                                tint = Color.White,
                                modifier = Modifier.size(48.dp)
                            )
                        }
                    }
                }

                /* center-left + center-right: rotate 180 (landscape only) */
                if (isLandscape) {
                    Box(
                        modifier = Modifier.fillMaxSize(),
                        contentAlignment = Alignment.CenterStart
                    ) {
                        IconButton(
                            onClick = { flipLandscape() },
                            modifier = Modifier.padding(start = 12.dp)
                                .size(56.dp)
                                .background(Color(0xCC000000), MaterialTheme.shapes.extraLarge)
                        ) {
                            Icon(
                                Icons.Filled.ScreenRotation,
                                contentDescription = stringResource(R.string.flip),
                                tint = Color.White,
                                modifier = Modifier.size(32.dp)
                            )
                        }
                    }
                    Box(
                        modifier = Modifier.fillMaxSize(),
                        contentAlignment = Alignment.CenterEnd
                    ) {
                        IconButton(
                            onClick = { flipLandscape() },
                            modifier = Modifier.padding(end = 12.dp)
                                .size(56.dp)
                                .background(Color(0xCC000000), MaterialTheme.shapes.extraLarge)
                        ) {
                            Icon(
                                Icons.Filled.ScreenRotation,
                                contentDescription = stringResource(R.string.flip),
                                tint = Color.White,
                                modifier = Modifier.size(32.dp)
                            )
                        }
                    }
                }

                /* bottom bar: seek + time */
                Row(
                    modifier = Modifier.fillMaxWidth().align(Alignment.BottomCenter)
                        .background(Color(0xAA000000))
                        .navigationBarsPadding()
                        .padding(horizontal = 8.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        formatDurationMs(if (dragging) dragPosition else positionMs),
                        color = Color.White, fontSize = 12.sp
                    )

                    val fraction = if (durationMs > 0)
                        ((if (dragging) dragPosition else positionMs).toFloat() / durationMs).coerceIn(0f, 1f)
                    else 0f

                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .height(32.dp)
                            .pointerInput(player, durationMs, playback.prepared, error) {
                                detectTapGestures { offset ->
                                    if (!playback.prepared || error != null) return@detectTapGestures
                                    onInteraction()
                                    val pos = (offset.x / size.width * durationMs).toInt().coerceIn(0, durationMs)
                                    player.seek(pos)
                                }
                            }
                            .pointerInput(player, durationMs, playback.prepared, error) {
                                detectDragGestures(
                                    onDragStart = { offset ->
                                        wasPlayingBeforeDrag = player.state.value.playing
                                        if (playback.prepared && error == null && (!wasPlayingBeforeDrag || player.pause())) {
                                            dragging = true
                                            dragPosition = (offset.x / size.width * durationMs).toInt().coerceIn(0, durationMs)
                                            player.seek(dragPosition)
                                        }
                                    },
                                    onDrag = { change, _ ->
                                        change.consume()
                                        if (dragging) {
                                            dragPosition = (change.position.x / size.width * durationMs).toInt().coerceIn(0, durationMs)
                                            player.seek(dragPosition)
                                        }
                                    },
                                    onDragEnd = {
                                        if (dragging) {
                                            dragging = false
                                            if (player.seek(dragPosition) && wasPlayingBeforeDrag) player.play()
                                        }
                                    },
                                    onDragCancel = {
                                        if (dragging) {
                                            dragging = false
                                            if (player.seek(dragPosition) && wasPlayingBeforeDrag) player.play()
                                        }
                                    }
                                )
                            }
                    ) {
                        Box(
                            Modifier.align(Alignment.CenterStart).fillMaxWidth().height(4.dp)
                                .background(Color.White.copy(alpha = 0.4f), RoundedCornerShape(2.dp))
                        )
                        Box(
                            Modifier.align(Alignment.CenterStart).fillMaxWidth(fraction).height(4.dp)
                                .background(Color.White, RoundedCornerShape(2.dp))
                        )
                        Box(
                            Modifier.align(Alignment.CenterStart).fillMaxWidth(fraction).height(32.dp),
                            contentAlignment = Alignment.CenterEnd
                        ) {
                            Box(Modifier.size(14.dp).background(Color.White, CircleShape))
                        }
                    }

                    Text(formatDurationMs(durationMs), color = Color.White, fontSize = 12.sp)
                }
            }
        }
    }
}

private fun formatDurationMs(ms: Int): String {
    val secs = (ms.coerceAtLeast(0)) / 1000
    return "%d:%02d".format(secs / 60, secs % 60)
}
