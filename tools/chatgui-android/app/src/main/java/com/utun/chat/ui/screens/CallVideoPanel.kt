package com.utun.chat.ui.screens

import android.content.Context
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.os.Build
import android.view.Surface
import android.view.TextureView
import android.view.ViewTreeObserver
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import kotlin.math.roundToInt
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.zIndex
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.PopupProperties
import com.utun.chat.data.LogManager
import com.utun.chat.data.CallController
import com.utun.chat.data.CallVideoBitrateRange
import com.utun.chat.data.CallVideoEngine
import com.utun.chat.data.CallVideoSettings
import com.utun.chat.data.CallVideoFormat
import com.utun.chat.data.StreamVideoCodec
import com.utun.chat.data.StreamPreviewTarget
import com.utun.chat.R

internal enum class CallVideoOption { CAMERA, CODEC, BITRATE, RESOLUTION }

/** Настройки собственной передачи; приём и прямое превью не управляют доступностью панели. */
@Composable
internal fun CallVideoToolbar(video: CallVideoEngine, menu: CallVideoOption?, onMenuChange: (CallVideoOption?) -> Unit,
                             onSource: (CallVideoSettings) -> Unit, onInteraction: (Boolean) -> Unit, modifier: Modifier = Modifier) {
    val state by video.state.collectAsState()
    val menuProperties = remember { PopupProperties(focusable = false, dismissOnClickOutside = false) }
    BackHandler(menu != null) { onMenuChange(null) }
    var dragging by remember(video) { mutableStateOf(false) }
    LaunchedEffect(menu) { if (menu != CallVideoOption.BITRATE) dragging = false }
    var selected by remember(video) { mutableStateOf(state.range.minimum.toFloat()..state.range.maximum.toFloat()) }
    LaunchedEffect(state.range, menu) {
        if (menu != CallVideoOption.BITRATE || !dragging) selected = state.range.minimum.toFloat()..state.range.maximum.toFloat()
    }
    val interaction by rememberUpdatedState(onInteraction)
    LaunchedEffect(menu, dragging) {
        interaction(menu != null || dragging)
        LogManager.addLog("DEBUG", "call", "video toolbar menu=$menu dragging=$dragging")
    }
    DisposableEffect(video) { onDispose { interaction(false) } }
    val camera = state.cameras.firstOrNull { it.id == state.settings.camera }
    val sourceLabel = stringResource(R.string.capture_video_source)
    val sourceName = if (state.settings.screen) stringResource(R.string.capture_screen) else camera?.name.orEmpty()
    val codecLabel = stringResource(R.string.call_video_codec)
    val resolutionLabel = stringResource(R.string.call_video_resolution)
    val codecName = when (state.settings.codec) { 0 -> stringResource(R.string.call_video_auto); 2 -> "H.265"; else -> "H.264" }
    val activeCodec = state.activeCodec.takeIf { state.transmitting && it != 0 }?.let { StreamVideoCodec.fromId(it).label }
    Column(modifier, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        if (state.transmitting && state.sharingScreen) Text(stringResource(R.string.capture_sharing_screen))
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Box {
                CaptureControlButton("$sourceLabel: $sourceName",
                    enabled = state.ready && state.codecs.isNotEmpty(),
                    onClick = { onMenuChange(if (menu == CallVideoOption.CAMERA) null else CallVideoOption.CAMERA) }) {
                    val focal = camera?.focal35?.takeIf { it > 0 }?.let { stringResource(R.string.stream_camera_focal_short, it.roundToInt()) }
                    CaptureValueIcon(if (state.settings.screen) CaptureIconShape.SCREEN else CaptureIconShape.CAMERA, focal.takeUnless { state.settings.screen } ?: "—", front = camera?.front == true)
                }
                DropdownMenu(menu == CallVideoOption.CAMERA, properties = menuProperties,
                    onDismissRequest = { if (menu == CallVideoOption.CAMERA) onMenuChange(null) }) {
                    CaptureMenuHeader(sourceLabel)
                    val screenModes = state.modes(screen = true)
                    if (screenModes.isNotEmpty()) CaptureOptionItem(stringResource(R.string.capture_screen), state.settings.screen, onClick = {
                        onMenuChange(null)
                        val mode = screenModes.firstOrNull { it.width == 1280 && it.height == 720 && it.fps == 30 } ?: screenModes.first()
                        if (!state.settings.screen || !state.settings.enabled) onSource(state.settings.copy(enabled = true,
                            screen = true, width = mode.width, height = mode.height, fps = mode.fps))
                    })
                    state.cameras.filter { state.modes(it.id, screen = false).isNotEmpty() }.forEach { option ->
                        CaptureOptionItem(option.name, !state.settings.screen && option.id == state.settings.camera, onClick = {
                            onMenuChange(null)
                            val modes = state.modes(option.id, screen = false)
                            val mode = modes.firstOrNull { it.width == state.settings.width && it.height == state.settings.height && it.fps == state.settings.fps }
                                ?: modes.firstOrNull { it.fps == state.settings.fps } ?: modes.first()
                            LogManager.addLog("INFO", "call", "video toolbar camera=${option.id} mode=$mode")
                            onSource(state.settings.copy(enabled = true, screen = false, camera = option.id, width = mode.width, height = mode.height, fps = mode.fps))
                        })
                    }
                }
            }
            Box {
                CaptureControlButton("$codecLabel: ${activeCodec ?: codecName}" +
                    if (activeCodec != null && state.settings.codec == 0) " ($codecName)" else "",
                    enabled = state.ready && state.codecs.isNotEmpty(),
                    onClick = { onMenuChange(if (menu == CallVideoOption.CODEC) null else CallVideoOption.CODEC) }) {
                    CaptureValueIcon(CaptureIconShape.CHIP,
                        activeCodec ?: if (state.settings.codec == 0) stringResource(R.string.automatic) else codecName)
                }
                DropdownMenu(menu == CallVideoOption.CODEC, properties = menuProperties,
                    onDismissRequest = { if (menu == CallVideoOption.CODEC) onMenuChange(null) }) {
                    CaptureMenuHeader(codecLabel)
                    for (codec in listOf(0) + state.codecs.sortedDescending()) {
                        val label = when (codec) { 0 -> stringResource(R.string.call_video_auto); 2 -> "H.265"; else -> "H.264" }
                        val modes = state.modes(codec = codec)
                        if (modes.isNotEmpty()) CaptureOptionItem(label, codec == state.settings.codec, onClick = {
                            onMenuChange(null)
                            val mode = modes.firstOrNull { it.width == state.settings.width && it.height == state.settings.height && it.fps == state.settings.fps }
                                ?: modes.firstOrNull { it.fps == state.settings.fps } ?: modes.first()
                            LogManager.addLog("INFO", "call", "video toolbar codec=$codec mode=$mode")
                            CallController.setVideo(state.settings.copy(codec = codec, width = mode.width, height = mode.height, fps = mode.fps))
                        })
                    }
                }
            }
            Box {
                val description = stringResource(R.string.call_video_rate, state.range.minimum, state.range.maximum, state.bitrate / 1000)
                CaptureControlButton(description, enabled = state.ready,
                    onClick = { onMenuChange(if (menu == CallVideoOption.BITRATE) null else CallVideoOption.BITRATE) }) {
                    CaptureValueIcon(CaptureIconShape.GAUGE, captureBitrateLabel(state.bitrate),
                        detail = "${state.range.minimum}–${state.range.maximum}")
                }
                DropdownMenu(menu == CallVideoOption.BITRATE, properties = menuProperties,
                    onDismissRequest = { if (menu == CallVideoOption.BITRATE) onMenuChange(null) }) {
                    CaptureMenuHeader(stringResource(R.string.capture_bitrate_title))
                    Column(Modifier.width(300.dp).padding(horizontal = 16.dp, vertical = 8.dp)) {
                        Text(stringResource(R.string.call_video_rate, selected.start.toInt(), selected.endInclusive.toInt(), state.bitrate / 1000))
                        RangeSlider(value = selected, onValueChange = { dragging = true; selected = it }, valueRange = 300f..4000f,
                            onValueChangeFinished = {
                                dragging = false
                                video.range(CallVideoBitrateRange(selected.start.toInt(), selected.endInclusive.toInt()))
                            })
                    }
                }
            }
            Box {
                CaptureControlButton("$resolutionLabel: ${state.settings.width}×${state.settings.height} · ${state.settings.fps} fps",
                    enabled = state.ready && state.sizes().isNotEmpty(),
                    onClick = { onMenuChange(if (menu == CallVideoOption.RESOLUTION) null else CallVideoOption.RESOLUTION) }) {
                    CaptureValueIcon(CaptureIconShape.SCREEN, "${minOf(state.settings.width, state.settings.height)}p", detail = "${state.settings.fps} fps")
                }
                DropdownMenu(menu == CallVideoOption.RESOLUTION, properties = menuProperties,
                    onDismissRequest = { if (menu == CallVideoOption.RESOLUTION) onMenuChange(null) }) {
                    CaptureMenuHeader(resolutionLabel)
                    state.modes().forEach { mode -> CaptureOptionItem("${mode.width}×${mode.height} · ${mode.fps} fps",
                        mode.width == state.settings.width && mode.height == state.settings.height && mode.fps == state.settings.fps, onClick = {
                            onMenuChange(null)
                            LogManager.addLog("INFO", "call", "video toolbar mode=$mode")
                            CallController.setVideo(state.settings.copy(width = mode.width, height = mode.height, fps = mode.fps))
                        }) }
                }
            }
        }
        val notice = state.notice ?: if (!state.ready) R.string.call_video_initializing
            else if (state.sizes().isEmpty()) R.string.call_video_unavailable else null
        notice?.let { Text(stringResource(it), color = Color(0xFFFFB4AB), style = MaterialTheme.typography.bodySmall) }
    }
}

internal enum class CallVideoSource { LOCAL, REMOTE }

/** Один TextureView меняет только геометрию: переход в миниатюру не переподключает decoder. */
@Composable
@OptIn(ExperimentalLayoutApi::class)
internal fun CallVideoStage(video: CallVideoEngine, expanded: CallVideoSource?, slot: Rect, headerBottom: Float,
    onSelect: (CallVideoSource) -> Unit, onTap: () -> Unit, modifier: Modifier = Modifier) {
    val remote by video.display.collectAsState()
    val local by video.preview.state.collectAsState()
    val state by video.state.collectAsState()
    val displayDegrees = StreamDisplayDegrees()
    val density = LocalDensity.current
    val localVisible = local.active || state.transmitting
    LaunchedEffect(state.activeCodec, remote?.codec) {
        LogManager.addLog("DEBUG", "call", "video codec labels tx=${state.activeCodec} rx=${remote?.codec ?: 0}")
    }
    BoxWithConstraints(modifier) {
        val gapPx = with(density) { 12.dp.toPx() }
        val count = (if (localVisible) 1 else 0) + (if (remote != null) 1 else 0)
        val inline = expanded == null && count > 0
        val frameWidth = if (inline) with(density) { ((slot.width - gapPx * (count - 1)).coerceAtLeast(0f) / count).toDp() }
            else minOf(104.dp, maxWidth / 2 - 12.dp).coerceAtLeast(0.dp)
        val frameHeight = if (inline) with(density) { slot.height.coerceAtLeast(0f).toDp() } else 144.dp
        val widthPx = with(density) { frameWidth.toPx() }
        val centerX = if (slot.width > 0) slot.center.x else with(density) { maxWidth.toPx() } / 2
        LaunchedEffect(expanded, slot, count, frameWidth, frameHeight) {
            LogManager.addLog("DEBUG", "call", "video layout expanded=$expanded visible=$count slot=$slot " +
                "frame=${frameWidth.value}x${frameHeight.value}dp; preview fits available space")
        }
        // Обе поверхности живут до конца видеосессии, даже пока аватар скрывает пустой слот.
        for (source in CallVideoSource.entries) key(video, source) {
            val large = expanded == source
            val index = if (source == CallVideoSource.REMOTE && localVisible) 1 else 0
            val x = if (expanded != null) with(density) { maxWidth.toPx() } - widthPx - gapPx
                else centerX - (maxOf(count, 1) * widthPx + maxOf(count - 1, 0) * gapPx) / 2 + index * (widthPx + gapPx)
            val y = if (expanded != null) headerBottom + gapPx else slot.top
            Box(Modifier.matchParentSize().zIndex(if (large) 0f else 1f)) {
                val frameModifier = if (large) Modifier.matchParentSize() else Modifier
                    .offset { IntOffset(x.roundToInt(), y.roundToInt()) }.size(frameWidth, frameHeight)
                val description = stringResource(if (source == CallVideoSource.LOCAL) R.string.call_video_local else R.string.call_video_remote)
                val click = { if (large) onTap() else onSelect(source) }
                if (source == CallVideoSource.REMOTE) {
                    CallVideoPlayback(video, click, frameModifier, description, large)
                } else BoxWithConstraints(frameModifier, contentAlignment = Alignment.Center) {
                    val aspect = local.format?.aspect(displayDegrees) ?: 1f
                    val fit = fitVideoFrame(maxWidth.value, maxHeight.value, aspect, large)
                    LaunchedEffect(fit, large) {
                        LogManager.addLog("DEBUG", "call", "video local fit expanded=$large aspect=$aspect fit=$fit")
                    }
                    AndroidView(factory = { CameraPreviewView(it, video.preview, keepAwake = false) },
                        modifier = Modifier.requiredSize(fit.width.dp, fit.height.dp).graphicsLayer { rotationZ = fit.rotation }, update = { view ->
                            view.update(local.format, displayDegrees); view.alpha = if (local.active) 1f else 0f
                            view.contentDescription = description
                            view.setOnClickListener { if (local.active) click() }; view.isClickable = local.active
                        }, onRelease = { it.close() })
                    if (localVisible && !local.active) Text(stringResource(if (local.failed) R.string.stream_preview_failed
                        else R.string.stream_connecting), color = Color.White, modifier = Modifier.padding(8.dp))
                    if (state.transmitting && state.activeCodec != 0) Text(StreamVideoCodec.fromId(state.activeCodec).label,
                        color = Color.White, style = MaterialTheme.typography.labelSmall,
                        modifier = Modifier.align(Alignment.BottomStart).background(Color.Black.copy(alpha = 0.65f)).padding(4.dp))
                }
            }
        }
    }
}

@Composable
private fun CallVideoPlayback(video: CallVideoEngine, onTap: () -> Unit, modifier: Modifier, description: String, large: Boolean) {
    val format by video.display.collectAsState()
    BoxWithConstraints(modifier) {
        val rotated = format?.rotation?.rem(180) != 0 && format != null
        val aspect = format?.let { if (rotated) it.height.toFloat() / it.width else it.width.toFloat() / it.height } ?: 1f
        val fit = fitVideoFrame(maxWidth.value, maxHeight.value, aspect, large)
        LaunchedEffect(fit, format, large) {
            LogManager.addLog("INFO", "call", "video remote fit expanded=$large viewport=${maxWidth.value}x${maxHeight.value} " +
                "format=$format fit=$fit; interface orientation retained")
        }
        key(video) {
            AndroidView(factory = { CallVideoTexture(it, video) }, modifier = Modifier.requiredSize(fit.width.dp, fit.height.dp)
                .align(Alignment.Center).graphicsLayer { rotationZ = fit.rotation },
                update = { view -> view.update(format, large); view.alpha = if (format == null) 0f else 1f
                    view.contentDescription = description
                    view.setOnClickListener { if (format != null) onTap() }; view.isClickable = format != null }, onRelease = { it.close() })
        }
        format?.let { Text(StreamVideoCodec.fromId(it.codec).label, color = Color.White, style = MaterialTheme.typography.labelSmall,
            modifier = Modifier.align(Alignment.BottomStart).background(Color.Black.copy(alpha = 0.65f)).padding(4.dp)) }
    }
}

private class CallVideoTexture(context: Context, private val video: CallVideoEngine) : TextureView(context), ViewTreeObserver.OnPreDrawListener {
    private var attached: Surface? = null
    private var attachedTexture: SurfaceTexture? = null
    private val ownedTextures = java.util.Collections.newSetFromMap(java.util.IdentityHashMap<SurfaceTexture, Boolean>())
    private var closed = false
    private var format: CallVideoFormat? = null
    private var large = false
    private var observer: ViewTreeObserver? = null
    private var reportAt = 0L
    private var updates = 0
    private var commits = 0
    private var updateAt = 0L
    private var commitMaxUs = 0L
    fun update(next: CallVideoFormat?, expanded: Boolean) {
        if (format == next && large == expanded) return
        format = next; large = expanded; fit()
    }
    init {
        isOpaque = false
        surfaceTextureListener = object : SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                try {
                    val target = StreamPreviewTarget(texture)
                    ownedTextures.add(texture)
                    if (closed) { target.release(); return }
                    closeOutput()
                    attached = target.surface; attachedTexture = texture
                    // RX worker передаёт Surface/texture в идемпотентный cleanup после остановки decoder.
                    video.surface(target.surface) { target.release() }; fit()
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "call", "video UI surface attach failed texture=${System.identityHashCode(texture)}: $e")
                }
            }
            override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) { fit() }
            override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                if (!ownedTextures.remove(texture)) return true
                if (attachedTexture === texture) closeOutput()
                return false
            }
            override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {
                updateAt = System.nanoTime(); updates++
            }
        }
    }
    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        observer = viewTreeObserver.also { it.addOnPreDrawListener(this) }
        LogManager.addLog("DEBUG", "call", "video presentation attached preDrawCommit=true hardware=$isHardwareAccelerated")
    }
    override fun onDetachedFromWindow() {
        observer?.takeIf { it.isAlive }?.removeOnPreDrawListener(this); observer = null
        super.onDetachedFromWindow()
    }
    override fun onPreDraw(): Boolean {
        val texture = surfaceTexture; val surface = attached
        if (!closed && texture != null && surface != null) {
            // ViewRoot собирает commit callbacks ДО TextureView.draw/onSurfaceTextureUpdated.
            // Регистрируем до draw, иначе ACK ждёт следующего постороннего обновления окна.
            if (Build.VERSION.SDK_INT >= 29 && isHardwareAccelerated)
                viewTreeObserver.registerFrameCommitCallback { consumed(texture, surface) }
            else postOnAnimation { consumed(texture, surface) }
        }
        return true
    }
    private fun consumed(texture: SurfaceTexture, surface: Surface) {
        if (closed || attached !== surface) return
        val now = System.nanoTime(); commits++
        if (updateAt != 0L) { commitMaxUs = maxOf(commitMaxUs, (now - updateAt) / 1000); updateAt = 0 }
        // Если timestamp ещё старый, новое TextureView update само назначит draw/commit.
        val matched = video.presented(surface, texture.timestamp / 1000)
        if (!matched && (Build.VERSION.SDK_INT < 29 || !isHardwareAccelerated)) postOnAnimation { consumed(texture, surface) }
        if (reportAt == 0L) reportAt = now
        if (now - reportAt >= 1_000_000_000) {
            LogManager.addLog("DEBUG", "call", "video surface intervalMs=${(now - reportAt) / 1_000_000} " +
                "updates=$updates commits=$commits updateToCommitMaxUs=$commitMaxUs pts=${texture.timestamp / 1000}")
            reportAt = now; updates = 0; commits = 0; commitMaxUs = 0
        }
    }
    private fun fit() {
        val f = format ?: return
        if (width == 0 || height == 0) return
        val rotated = f.rotation % 180 != 0
        val w = if (rotated) f.height else f.width; val h = if (rotated) f.width else f.height
        val scale = minOf(width.toFloat() / w, height.toFloat() / h)
        setTransform(Matrix().apply {
            setScale(w * scale / width, h * scale / height, width / 2f, height / 2f)
        })
        com.utun.chat.data.LogManager.addLog("DEBUG", "call", "video fit view=${width}x$height frame=${w}x$h expanded=$large")
    }
    private fun closeOutput() { attached?.let(video::detachSurface); attached = null; attachedTexture = null }
    fun close() {
        closed = true
        closeOutput()
    }
}
