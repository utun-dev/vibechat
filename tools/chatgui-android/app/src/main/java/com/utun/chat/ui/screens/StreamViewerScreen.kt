package com.utun.chat.ui.screens

import com.utun.chat.ui.components.MediaAudioDeviceButton

import android.graphics.SurfaceTexture
import android.view.TextureView
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.utun.chat.R
import com.utun.chat.data.LogManager
import com.utun.chat.data.StreamPlayerEngine
import com.utun.chat.data.StreamPreviewTarget
import com.utun.chat.data.StreamPlaybackTelemetry
import kotlinx.coroutines.delay

/** Недоступность заменяет только текущий статус ожидания/показа; завершение и ошибки reader сохраняются. */
internal fun streamViewerDisplayStatus(status: Int, sourceAvailable: Boolean?): Int {
    if (sourceAvailable != false) return status
    return when (status) {
        R.string.stream_connecting, R.string.stream_live, R.string.stream_waiting_key,
        R.string.stream_waiting_decoder -> R.string.stream_waiting_source
        else -> status
    }
}

/** Полноэкранный viewer: видео разворачивается вдоль окна, ориентацию экрана не меняем.
 * sourceAvailable=null — каталог ещё не подтверждён; false меняет подпись и затемняет видео, сохраняя reader и буфер. */
@Composable
internal fun StreamViewerScreen(group: String, source: String, epoch: Long, title: String, name: String,
                                sourceAvailable: Boolean?, onClose: () -> Unit) {
    ViewerFullscreenWindow("stream source=$source epoch=$epoch")
    val owner = LocalLifecycleOwner.current
    var visible by remember(owner) { mutableStateOf(owner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    var aspect by remember { mutableStateOf(0f) }
    var status by remember { mutableStateOf(R.string.stream_connecting) }
    val displayStatus = streamViewerDisplayStatus(status, sourceAvailable)
    var telemetry by remember { mutableStateOf(StreamPlaybackTelemetry()) }
    var controls by remember { mutableStateOf(true) }
    var hideTick by remember { mutableStateOf(0) }
    val zoom = remember { VideoZoomState() }

    DisposableEffect(owner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_START) { visible = true; controls = true }
            if (event == Lifecycle.Event.ON_STOP) visible = false
        }
        owner.lifecycle.addObserver(observer)
        onDispose { owner.lifecycle.removeObserver(observer) }
    }
    LaunchedEffect(sourceAvailable) {
        LogManager.addLog("INFO", "stream", "viewer availability group=$group source=$source epoch=$epoch " +
            "available=$sourceAvailable preserveViewer=true")
    }
    LaunchedEffect(displayStatus) {
        controls = true
        LogManager.addLog("INFO", "stream", "viewer display group=$group source=$source epoch=$epoch " +
            "status=$displayStatus dimmed=${displayStatus == R.string.stream_waiting_source}")
        if (status == R.string.stream_ended) {
            LogManager.addLog("INFO", "stream", "viewer ended; closing window group=$group source=$source epoch=$epoch")
            onClose()
        }
    }
    var audioMenu by remember { mutableStateOf(false) }
    LaunchedEffect(controls, displayStatus, visible, hideTick, audioMenu) {
        if (controls && displayStatus == R.string.stream_live && visible && !audioMenu) { delay(3000); controls = false }
    }
    LaunchedEffect(controls) { LogManager.addLog("DEBUG", "stream", "viewer controls source=$source visible=$controls") }
    LaunchedEffect(zoom.scale, zoom.offset) {
        delay(300)
        LogManager.addLog("DEBUG", "stream", "viewer zoom source=$source scale=${zoom.scale} offset=${zoom.offset}")
    }

    BoxWithConstraints(Modifier.fillMaxSize().background(Color.Black).clipToBounds()) {
        val ratio = aspect.takeIf { it > 0f } ?: 16f / 9f
        val fit = fitVideoFrame(maxWidth.value, maxHeight.value, ratio, aspect > 0f)
        val turn = fit.rotation != 0f
        val width = fit.width.dp; val height = fit.height.dp
        val density = LocalDensity.current
        // Жесты работают в координатах окна, поэтому учитывают размеры после поворота видео.
        val videoSize = with(density) { if (turn) Size(height.toPx(), width.toPx()) else Size(width.toPx(), height.toPx()) }
        val viewport = with(density) { Size(maxWidth.toPx(), maxHeight.toPx()) }
        LaunchedEffect(aspect, turn, videoSize, viewport) {
            LogManager.addLog("INFO", "stream", "viewer fit source=$source aspect=$aspect viewport=$viewport " +
                "video=$videoSize videoRotation=${if (turn) 90 else 0}")
        }
        LaunchedEffect(videoSize, viewport) { zoom.constrain(videoSize, viewport) }
        if (visible) StreamViewerPlayback(group, source, epoch, { aspect = it }, { status = it },
            Modifier.requiredSize(width, height).align(Alignment.Center).graphicsLayer {
                rotationZ = if (turn) 90f else 0f
                scaleX = zoom.scale; scaleY = zoom.scale
                translationX = zoom.offset.x; translationY = zoom.offset.y
            }, onTelemetry = { telemetry = it })
        Box(Modifier.matchParentSize()
            .background(if (displayStatus == R.string.stream_waiting_source) Color.Black.copy(alpha = 0.6f) else Color.Transparent)
            .pointerInput(displayStatus) {
                detectTapGestures { if (displayStatus == R.string.stream_live) controls = !controls else controls = true }
            }
            .pointerInput(videoSize, viewport) {
                detectTransformGestures { centroid, pan, factor, _ ->
                    controls = true; hideTick++
                    zoom.transform(centroid, pan, factor, videoSize, viewport)
                }
            })
        StreamPlaybackIndicator(telemetry, Modifier.align(Alignment.BottomStart).fillMaxWidth()
            .windowInsetsPadding(WindowInsets.safeDrawing), showModeSwitch = true)
        if (controls) {
            Row(Modifier.fillMaxWidth().align(Alignment.TopCenter).background(Color.Black.copy(alpha = 0.325f))
                .windowInsetsPadding(WindowInsets.safeDrawing).padding(start = 16.dp, end = 4.dp, top = 4.dp, bottom = 4.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(title, color = Color.White, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(name, color = Color.White, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(stringResource(displayStatus), color = Color.White, style = MaterialTheme.typography.bodySmall)
                }
                MediaAudioDeviceButton(tint = Color.White, onExpandedChange = { audioMenu = it })
                IconButton(onClick = onClose) { Icon(Icons.Filled.Close, stringResource(R.string.stream_close_view), tint = Color.White) }
            }
        }
    }
}

/** Смена размеров окна и скрытие controls сохраняют TextureView, decoder и сетевой reader.
 * Удаление из composition останавливает reader; target передаётся cleanup worker до освобождения decoder/DSP. */
@Composable
internal fun StreamViewerPlayback(group: String, source: String, epoch: Long, onAspect: (Float) -> Unit, onStatus: (Int) -> Unit,
                                 modifier: Modifier, onEngine: (StreamPlayerEngine) -> Unit = {},
                                 onTelemetry: (StreamPlaybackTelemetry) -> Unit = {}) {
    var engine by remember(group, source, epoch) { mutableStateOf<StreamPlayerEngine?>(null) }
    var startupError by remember(group, source, epoch) { mutableStateOf(false) }
    val aspect = engine?.aspect?.collectAsState()?.value ?: 0f
    val engineStatus = engine?.status?.collectAsState()?.value ?: R.string.stream_connecting
    val status = if (startupError) R.string.stream_failed else engineStatus
    val telemetry = engine?.telemetry?.collectAsState()?.value ?: StreamPlaybackTelemetry()
    val resources = remember(group, source, epoch) { StreamViewerSurface() }
    fun releaseSurface(texture: SurfaceTexture? = null) {
        val target = resources.target ?: return
        if (texture != null && texture !== target.texture) return
        resources.target = null
        val currentEngine = resources.engine
        resources.engine = null
        LogManager.addLog("INFO", "stream", "viewer surface release source=$source epoch=$epoch texture=${System.identityHashCode(target.texture)}")
        if (currentEngine != null) currentEngine.stop { target.release() } else target.release()
    }
    LaunchedEffect(aspect, status) { onAspect(aspect); onStatus(status) }
    LaunchedEffect(telemetry) { onTelemetry(telemetry) }
    DisposableEffect(resources) { onDispose { resources.disposed = true; releaseSurface() } }
    key(resources) { AndroidView(factory = { context ->
        TextureView(context).apply {
            keepScreenOn = true
            surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                    if (resources.disposed) { StreamPreviewTarget(texture).release(); return }
                    releaseSurface()
                    val player = StreamPlayerEngine(group, source, epoch)
                    val target = StreamPreviewTarget(texture)
                    resources.target = target; resources.engine = player
                    LogManager.addLog("INFO", "stream", "viewer surface available source=$source epoch=$epoch " +
                        "texture=${System.identityHashCode(texture)} size=${width}x$height")
                    engine = player
                    try { onEngine(player); player.start(target.surface) } catch (e: Exception) {
                        LogManager.addLog("ERROR", "stream", "viewer UI start failed source=$source epoch=$epoch: $e")
                        startupError = true; releaseSurface()
                    }
                }
                override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {
                    LogManager.addLog("DEBUG", "stream", "viewer surface resized source=$source " +
                        "texture=${System.identityHashCode(texture)} size=${width}x$height")
                }
                override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                    releaseSurface(texture); return false
                }
                override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
            }
        }
    }, modifier = modifier, onRelease = { resources.disposed = true; releaseSurface() }) }
}

/** TextureView передаёт освобождение texture приложению; repeated dispose не закрывает новый decoder. */
private class StreamViewerSurface {
    var disposed = false
    var engine: StreamPlayerEngine? = null
    var target: StreamPreviewTarget? = null
}
