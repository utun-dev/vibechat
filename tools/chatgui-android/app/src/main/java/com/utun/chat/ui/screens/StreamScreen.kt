package com.utun.chat.ui.screens

import com.utun.chat.ui.components.MediaAudioDeviceButton

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.content.Context
import android.content.ContextWrapper
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.TextureView
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.VolumeOff
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.PopupProperties
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.utun.chat.R
import com.utun.chat.ChatApplication
import com.utun.chat.data.LogManager
import com.utun.chat.data.StreamController
import com.utun.chat.data.StreamPhase
import com.utun.chat.data.StreamPreview
import com.utun.chat.data.StreamPreviewFormat
import com.utun.chat.data.StreamPreviewTarget
import com.utun.chat.data.StreamPreviewViewResources
import com.utun.chat.data.StreamSource
import com.utun.chat.data.StreamCaptureMode
import com.utun.chat.data.StreamCaptureConfig
import com.utun.chat.data.StreamCaptureSettings
import com.utun.chat.data.StreamCamera
import com.utun.chat.data.StreamCameraModes
import com.utun.chat.data.StreamBufferPositions
import com.utun.chat.data.LensType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt

/** Выход закрывает reader либо прямой preview output; foreground publisher останавливается явно. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun StreamScreen(channelId: String, channelName: String, sources: List<StreamSource>, selected: StreamSource?,
                 sourcesFailed: Boolean, onSelect: (StreamSource?) -> Unit, viewAll: Boolean = false,
                 onBack: () -> Unit) {
    val group = channelId
    if (viewAll) {
        var manageOwn by rememberSaveable(group) { mutableStateOf(false) }
        LaunchedEffect(manageOwn) {
            LogManager.addLog("INFO", "stream", "multiview controls group=$group open=$manageOwn " +
                "publisher=${StreamController.state.value?.id} readersPreserved=true")
        }
        Box(Modifier.fillMaxSize()) {
            StreamGridScreen(group, channelName, sources, sourcesFailed, ownPreviewVisible = !manageOwn, onOwnStream = {
                val publisher = StreamController.state.value
                if (publisher?.ongoing == true && publisher.group == group) manageOwn = true
                else LogManager.addLog("WARN", "stream", "multiview controls stale group=$group publisher=${publisher?.id} phase=${publisher?.phase}")
            }, onClose = { onSelect(null); onBack() })
            if (manageOwn) {
                BackHandler { manageOwn = false }
                StreamPublishScreen(group, channelName) { manageOwn = false }
            }
        }
        return
    }
    if (selected != null && !selected.self) {
        key(group, selected.id, selected.epoch) {
            var sourceAvailable by remember { mutableStateOf<Boolean?>(null) }
            LaunchedEffect(sources, sourcesFailed) {
                if (!sourcesFailed) sourceAvailable = sources.any { it.id == selected.id && it.epoch == selected.epoch }
            }
            StreamViewerScreen(group, selected.id, selected.epoch, channelName, selected.displayName, sourceAvailable) {
                onSelect(null); onBack()
            }
        }
        return
    }
    StreamPublishScreen(group, channelName, onBack)
}

/** Управление публикацией используется отдельно и поверх сетки, не меняя выбор просмотра. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun StreamPublishScreen(group: String, channelName: String, onBack: () -> Unit) {
    val context = LocalContext.current
    val displayDegrees = StreamDisplayDegrees()
    val state by StreamController.state.collectAsState()
    val txStats by StreamController.txStats.collectAsState()
    val local = state?.takeIf { it.group == group }
    var audio by remember { mutableStateOf(true) }
    var config by remember { mutableStateOf<StreamCaptureConfig?>(null) }
    var cameras by remember { mutableStateOf<List<StreamCamera>>(emptyList()) }
    var screenModes by remember { mutableStateOf(emptyList<StreamCaptureMode>()) }
    val requestScreen = rememberScreenConsent(group to state?.id)
    var loadingModes by remember { mutableStateOf(true) }
    var menu by remember(group) { mutableStateOf<StreamOption?>(null) }
    var permissionError by remember(group) { mutableStateOf<Int?>(null) }
    LaunchedEffect(group) {
        val saved = try { ChatApplication.instance.configProvider.getStreamCaptureSettings() }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) {
            LogManager.addLog("ERROR", "stream", "capture settings load failed: $e"); StreamCaptureSettings()
        }
        try { cameras = withContext(Dispatchers.Default) { StreamCameraModes.enumerate(context) } }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { LogManager.addLog("ERROR", "video", "capture modes failed: ${e.message}") }
        val current = StreamController.state.value?.takeIf { it.group == group && it.ongoing }
        config = current?.config ?: saved.resolve(cameras)
        audio = current?.audioAvailable ?: saved.audio
        loadingModes = false
    }
    val screenMode = local?.config?.mode?.takeIf { local.ongoing && it.screen }
    val portrait = screenMode?.let { it.height > it.width }
        ?: (LocalConfiguration.current.orientation == android.content.res.Configuration.ORIENTATION_PORTRAIT)
    LaunchedEffect(group, portrait) {
        try {
            screenModes = withContext(Dispatchers.Default) { StreamCameraModes.screenModes(portrait) }
            val selected = config
            if (local?.ongoing != true && selected?.mode?.screen == true && selected.mode !in screenModes) {
                val next = screenModes.firstOrNull { it.videoCodec == selected.mode.videoCodec && it.width == selected.mode.height }
                    ?: screenModes.firstOrNull()
                config = next?.let { selected.withMode(it) }
            }
        } catch (e: CancellationException) { throw e }
        catch (e: Exception) { LogManager.addLog("ERROR", "video", "screen modes unavailable: $e") }
    }
    var permissionVersion by remember { mutableStateOf(0) }
    var cameraSwitch by remember(group) { mutableStateOf<Pair<Long, StreamCaptureConfig>?>(null) }
    val permissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
        val switch = cameraSwitch; cameraSwitch = null
        if (switch != null && granted[Manifest.permission.CAMERA] == true && StreamController.state.value?.id == switch.first)
            StreamController.change(switch.second)
        permissionVersion++
        val denied = granted.filterValues { !it }.keys
        permissionError = when {
            denied.isEmpty() -> null
            Manifest.permission.CAMERA in denied -> R.string.camera_permission_required
            else -> R.string.microphone_access_required
        }
        LogManager.addLog(if (denied.isEmpty()) "INFO" else "WARN", "stream", "capture permission result group=$group denied=$denied")
    }
    val lifecycle = LocalLifecycleOwner.current
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event -> if (event == Lifecycle.Event.ON_RESUME) permissionVersion++ }
        lifecycle.lifecycle.addObserver(observer)
        onDispose { lifecycle.lifecycle.removeObserver(observer) }
    }
    val cameraGranted = permissionVersion.let { ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED }
    val micGranted = permissionVersion.let { ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED }

    LaunchedEffect(local?.config, local?.audioAvailable) {
        local?.config?.let { config = it; audio = local.audioAvailable }
    }

    val ongoing = local?.takeIf { it.ongoing }
    LaunchedEffect(group, local?.id, local?.phase) {
        LogManager.addLog("INFO", "stream", "publish screen group=$group publisher=${local?.id} phase=${local?.phase} " +
            "directPreview=${ongoing != null} watchChoices=false")
    }
    val shownConfig = if (ongoing != null) ongoing.config ?: ongoing.pendingConfig else config
    val shownAudio = ongoing?.audioEnabled ?: audio
    LaunchedEffect(local?.phase, local?.pendingConfig, local?.id, shownConfig, shownAudio, displayDegrees) {
        LogManager.addLog("DEBUG", "stream", "screen group=$group phase=${local?.phase} publisher=${local?.id} " +
            "config=$shownConfig audio=$shownAudio display=$displayDegrees pending=${local?.pendingConfig}")
    }

    val title = stringResource(R.string.stream_channel_title, channelName)
    Scaffold(modifier = Modifier.pointerInput(menu) { if (menu != null) detectTapGestures { menu = null } }, topBar = {
        TopAppBar(title = {
            TooltipBox(positionProvider = TooltipDefaults.rememberPlainTooltipPositionProvider(),
                tooltip = { PlainTooltip { Text(title) } }, state = rememberTooltipState()) {
                Text(title, style = MaterialTheme.typography.titleMedium, fontSize = 18.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }, navigationIcon = {
            IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back)) }
        }, actions = {
            StreamBufferIndicator(txStats?.takeIf { it.id == ongoing?.id }?.buffer ?: StreamBufferPositions())
            Spacer(Modifier.width(4.dp))
            val action = if (ongoing == null) R.string.stream_start
                else if (ongoing.phase == StreamPhase.STOPPING) R.string.stream_stopping else R.string.stream_stop
            CaptureControlButton(stringResource(action),
                enabled = if (ongoing != null) ongoing.phase != StreamPhase.STOPPING
                    else !loadingModes && config != null && group.isNotEmpty() && state?.ongoing != true,
                containerColor = if (ongoing != null) Color(0xFFB32632) else Color(0xFF24633D), onClick = {
                    menu = null
                    if (ongoing != null) {
                        LogManager.addLog("INFO", "stream", "screen stop group=$group id=${ongoing.id} phase=${ongoing.phase}")
                        StreamController.stop()
                    } else if ((config?.mode?.screen != true && !cameraGranted) || (audio && !micGranted)) {
                        LogManager.addLog("INFO", "stream", "screen permissions group=$group camera=$cameraGranted mic=$micGranted audio=$audio")
                        try {
                            permissions.launch((if (config?.mode?.screen == true) emptyList() else listOf(Manifest.permission.CAMERA))
                                .plus(if (audio) listOf(Manifest.permission.RECORD_AUDIO) else emptyList()).toTypedArray())
                        } catch (e: Exception) {
                            LogManager.addLog("ERROR", "stream", "Cannot request capture permissions group=$group: $e")
                            permissionError = R.string.stream_failed
                        }
                    } else config?.let {
                        permissionError = null
                        LogManager.addLog("INFO", "stream", "screen start group=$group audio=$audio config=$it")
                        val before = StreamController.state.value?.id
                        if (it.mode.screen) requestScreen { grant ->
                            if (StreamController.state.value?.id == before) StreamController.start(group, it, audio, grant)
                        } else StreamController.start(group, it, audio)
                    }
                }) {
                Icon(if (ongoing != null) Icons.Filled.Stop else Icons.Filled.PlayArrow,
                    contentDescription = null, modifier = Modifier.size(32.dp))
            }
            Spacer(Modifier.width(4.dp))
        })
    }) { padding ->
        Column(Modifier.fillMaxSize().padding(padding).padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            permissionError?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error) }
            StreamCaptureToolbar(cameras, screenModes, shownConfig, displayDegrees, Modifier.fillMaxWidth(),
                menu = menu, onMenuChange = { menu = it },
                enabled = if (ongoing != null) ongoing.phase == StreamPhase.LIVE && ongoing.pendingConfig == null else !loadingModes,
                audio = shownAudio,
                audioEnabled = if (ongoing != null) ongoing.phase == StreamPhase.LIVE && ongoing.audioAvailable else !loadingModes,
                onAudioChange = {
                    LogManager.addLog("INFO", "stream", "screen audio group=$group enabled=$it live=${ongoing != null}")
                    if (ongoing != null) StreamController.setAudioEnabled(it)
                    else { audio = it; StreamController.rememberSettings(audio = it) }
                }, onChange = {
                    LogManager.addLog("INFO", "stream", "screen capture selection group=$group live=${ongoing != null} config=$it")
                    if (ongoing != null && it.mode.screen && ongoing.config?.mode?.screen != true) requestScreen { grant ->
                        if (StreamController.state.value?.id == ongoing.id) StreamController.change(it, grant)
                    } else if (ongoing != null && !it.mode.screen && !cameraGranted) {
                        cameraSwitch = ongoing.id to it
                        try { permissions.launch(arrayOf(Manifest.permission.CAMERA)) } catch (e: Exception) {
                            cameraSwitch = null; permissionError = R.string.camera_permission_required
                            LogManager.addLog("ERROR", "stream", "cannot request live camera permission group=$group: $e")
                        }
                    } else if (ongoing != null) StreamController.change(it)
                    else { config = it; StreamController.rememberSettings(config = it) }
                })
            if (ongoing != null) {
                CaptureModeStatus(ongoing.config, ongoing.pendingConfig, ongoing.notice)
                if (ongoing.phase == StreamPhase.STOPPING) Text(stringResource(R.string.stream_stopping))
                StreamTxPanel(ongoing.id)
            }
            if (ongoing != null) {
                Box(Modifier.weight(1f).fillMaxWidth()) {
                    if (ongoing.config?.mode?.screen == true) Text(stringResource(R.string.capture_sharing_screen))
                    else key(ongoing.id) { LiveStreamView(ongoing.preview, displayDegrees) }
                }
            } else {
                if (!loadingModes && cameras.isEmpty() && screenModes.isEmpty())
                    Text(stringResource(R.string.stream_no_modes), color = MaterialTheme.colorScheme.error)
                local?.error?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error) }
            }
        }
    }
}
@Composable
private fun CaptureModeStatus(config: StreamCaptureConfig?, pending: StreamCaptureConfig?, notice: Int?) {
    if (pending != null) Text(stringResource(if (config == null) R.string.stream_connecting else R.string.stream_switching))
    notice?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error) }
}

@Composable
private fun StreamTxPanel(id: Long) {
    val snapshot by StreamController.txStats.collectAsState()
    val s = snapshot?.takeIf { it.id == id } ?: return
    Column {
        if (s.sourceOverloaded) Text(stringResource(R.string.stream_source_overloaded), color = MaterialTheme.colorScheme.error)
        Text(stringResource(R.string.stream_tx_queues, s.videoBytes / 1024.0, s.inflightBytes / 1024.0),
            style = MaterialTheme.typography.bodySmall)
        Text(stringResource(R.string.stream_tx_total, s.totalBytes / 1024.0, s.acked1s / 1024.0, s.acked1s * 8 / 1_000_000.0),
            style = MaterialTheme.typography.bodySmall)
    }
}

private enum class StreamOption { AUDIO_DEVICE, CAMERA, CODEC, BITRATE, RESOLUTION, ORIENTATION }

/** Одна строка управления: значки отражают применённый режим, меню сохраняют все варианты выбора. */
@Composable
private fun StreamCaptureToolbar(cameras: List<StreamCamera>, screenModes: List<StreamCaptureMode>, config: StreamCaptureConfig?, display: Int, modifier: Modifier,
                                 menu: StreamOption?, onMenuChange: (StreamOption?) -> Unit,
                                 enabled: Boolean, audio: Boolean, audioEnabled: Boolean, onAudioChange: (Boolean) -> Unit,
                                 onChange: (StreamCaptureConfig) -> Unit) {
    val mode = config?.mode
    val camera = cameras.firstOrNull { it.id == mode?.camera }
    val allModes = if (mode?.screen == true) (screenModes + mode).distinct() else camera?.modes.orEmpty()
    val modes = allModes.filter { it.videoCodec == mode?.videoCodec }
    val menuProperties = remember { PopupProperties(focusable = false, dismissOnClickOutside = false) }
    BackHandler(menu != null) { onMenuChange(null) }
    LaunchedEffect(enabled) { if (!enabled) onMenuChange(null) }
    LaunchedEffect(menu) { LogManager.addLog("DEBUG", "stream", "toolbar menu=$menu enabled=$enabled manualBitrateRange=${mode?.manualBitrateRange}") }
    val shortBitrate = config?.let { captureBitrateLabel(it.bitrate) } ?: "—"
    val bitrateValue = config?.let { stringResource(R.string.stream_bitrate_value, it.bitrate / 1_000_000.0) } ?: "—"
    val bitrateDescription = if (config != null && config.videoBitrate == null)
        stringResource(R.string.stream_bitrate_auto_value, bitrateValue) else stringResource(R.string.stream_bitrate, bitrateValue)
    Row(modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically) {
        MediaAudioDeviceButton(expanded = menu == StreamOption.AUDIO_DEVICE, properties = menuProperties, onExpandedChange = {
            if (it) onMenuChange(StreamOption.AUDIO_DEVICE) else if (menu == StreamOption.AUDIO_DEVICE) onMenuChange(null)
        })
        CaptureControlButton(stringResource(if (audio) R.string.stream_audio_on else R.string.stream_audio_off),
            enabled = audioEnabled, checked = audio, onClick = { onMenuChange(null); onAudioChange(!audio) }) {
            Icon(if (audio) Icons.AutoMirrored.Filled.VolumeUp else Icons.AutoMirrored.Filled.VolumeOff,
                contentDescription = null, modifier = Modifier.size(32.dp))
        }
        Box {
            val name = if (mode?.screen == true) stringResource(R.string.capture_screen) else camera?.let { "${it.lens.label} ${it.lens.mm}".trim() } ?: "—"
            CaptureControlButton("${stringResource(R.string.capture_video_source)}: $name", enabled && (cameras.isNotEmpty() || screenModes.isNotEmpty()),
                onClick = { onMenuChange(if (menu == StreamOption.CAMERA) null else StreamOption.CAMERA) }) {
                val focal = camera?.lens?.eqFocal?.takeIf { it > 0 }?.let {
                    stringResource(R.string.stream_camera_focal_short, it.roundToInt())
                } ?: "—"
                CaptureValueIcon(if (mode?.screen == true) CaptureIconShape.SCREEN else CaptureIconShape.CAMERA, focal, front = camera?.lens?.type == LensType.SELFIE)
            }
            DropdownMenu(expanded = menu == StreamOption.CAMERA && enabled, properties = menuProperties,
                onDismissRequest = { if (menu == StreamOption.CAMERA) onMenuChange(null) }) {
                CaptureMenuHeader(stringResource(R.string.capture_video_source))
                if (screenModes.isNotEmpty()) CaptureOptionItem(stringResource(R.string.capture_screen), mode?.screen == true, onClick = {
                    onMenuChange(null)
                    val next = screenModes.firstOrNull { minOf(it.width, it.height) == 720 } ?: screenModes.first()
                    onChange(StreamCaptureConfig(next))
                })
                cameras.forEach { option ->
                    CaptureOptionItem("${option.lens.label} ${option.lens.mm}".trim(), option.id == mode?.camera, onClick = {
                        onMenuChange(null)
                        val next = StreamCameraModes.choose(option.modes, mode)
                        if (next != null) onChange(config?.withMode(next) ?: StreamCaptureConfig(next))
                        else LogManager.addLog("ERROR", "video", "camera selection has no compatible mode camera=${option.id}")
                    })
                }
            }
        }
        Box {
            CaptureControlButton(stringResource(R.string.stream_video_codec, mode?.videoCodec?.label ?: "—"),
                enabled && config != null, onClick = { onMenuChange(if (menu == StreamOption.CODEC) null else StreamOption.CODEC) }) {
                CaptureValueIcon(CaptureIconShape.CHIP, mode?.videoCodec?.name ?: "—")
            }
            DropdownMenu(expanded = menu == StreamOption.CODEC && enabled, properties = menuProperties,
                onDismissRequest = { if (menu == StreamOption.CODEC) onMenuChange(null) }) {
                CaptureMenuHeader(stringResource(R.string.call_video_codec))
                allModes.map { it.videoCodec }.distinct().sortedBy { it.id }.forEach { codec ->
                    CaptureOptionItem(codec.label, codec == mode?.videoCodec, onClick = {
                        onMenuChange(null)
                        val next = StreamCameraModes.choose(allModes.filter { it.videoCodec == codec }, mode)
                        if (next != null) config?.let { onChange(it.withMode(next)) }
                        else LogManager.addLog("ERROR", "video", "codec selection has no compatible mode camera=${camera?.id} codec=$codec")
                    })
                }
            }
        }
        Box {
            CaptureControlButton(bitrateDescription, enabled && config != null,
                onClick = { onMenuChange(if (menu == StreamOption.BITRATE) null else StreamOption.BITRATE) }) {
                CaptureValueIcon(CaptureIconShape.GAUGE, shortBitrate,
                    detail = if (config != null && config.videoBitrate == null) stringResource(R.string.stream_bitrate_auto_mark) else null)
            }
            DropdownMenu(expanded = menu == StreamOption.BITRATE && enabled, properties = menuProperties,
                onDismissRequest = { if (menu == StreamOption.BITRATE) onMenuChange(null) }) {
                CaptureMenuHeader(stringResource(R.string.capture_bitrate_title))
                CaptureOptionItem(stringResource(R.string.stream_bitrate_auto), config?.videoBitrate == null, onClick = {
                    onMenuChange(null); config?.let { onChange(it.copy(videoBitrate = null)) }
                })
                mode?.takeUnless { it.manualBitrateRange.isEmpty() }?.let { m ->
                    listOf(256000, 512000, 1000000, 2000000, 4000000, 6000000, 8000000, 12000000)
                        .map { it.coerceIn(m.manualBitrateRange) }.distinct().forEach { value ->
                            CaptureOptionItem(stringResource(R.string.stream_bitrate_value, value / 1_000_000.0),
                                config?.videoBitrate == value, onClick = {
                                    onMenuChange(null); config?.let { onChange(it.copy(videoBitrate = value)) }
                                })
                        }
                }
            }
        }
        Box {
            CaptureControlButton(stringResource(R.string.stream_resolution, mode?.label ?: "—"), enabled && modes.isNotEmpty(),
                onClick = { onMenuChange(if (menu == StreamOption.RESOLUTION) null else StreamOption.RESOLUTION) }) {
                CaptureValueIcon(CaptureIconShape.SCREEN, mode?.let { "${minOf(it.width, it.height)}p" } ?: "—",
                    detail = mode?.let { "${it.fps}fps" })
            }
            DropdownMenu(expanded = menu == StreamOption.RESOLUTION && enabled, properties = menuProperties,
                onDismissRequest = { if (menu == StreamOption.RESOLUTION) onMenuChange(null) }) {
                CaptureMenuHeader(stringResource(R.string.call_video_resolution))
                modes.forEach { option -> CaptureOptionItem(option.label, option == mode, onClick = {
                    onMenuChange(null); config?.let { onChange(it.withMode(option)) }
                }) }
            }
        }
        CaptureOrientationControl(config?.orientation, display, camera?.lens?.type == LensType.SELFIE,
            enabled = enabled && config != null && !config.mode.screen, expanded = menu == StreamOption.ORIENTATION, properties = menuProperties,
            onExpandedChange = { onMenuChange(if (it) StreamOption.ORIENTATION else null) },
            onChange = { option -> config?.let { onChange(it.copy(orientation = option)) } })
    }
}

@Composable
private fun LiveStreamView(localPreview: StreamPreview, displayDegrees: Int) {
    var aspect by remember(localPreview) { mutableStateOf(9f / 16f) }
    var status by remember(localPreview) { mutableStateOf(R.string.stream_connecting) }
    BoxWithConstraints(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        val fit = fitVideoFrame(maxWidth.value, maxHeight.value, aspect, false)
        StreamPreviewPlayback(localPreview, displayDegrees, onAspect = { aspect = it }, onStatus = { status = it },
            modifier = Modifier.requiredSize(fit.width.dp, fit.height.dp))
        if (status != R.string.stream_live) {
            Surface(Modifier.align(Alignment.TopCenter).padding(8.dp), shape = RoundedCornerShape(8.dp),
                color = MaterialTheme.colorScheme.surface.copy(alpha = 0.9f)) {
                Text(stringResource(status), Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                    color = if (status == R.string.stream_preview_failed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface)
            }
        }
    }
}

/** Общий прямой preview для экрана публикации и сетки: только Camera2 output, без reader/decoder/audio. */
@Composable
internal fun StreamPreviewPlayback(localPreview: StreamPreview, displayDegrees: Int, onAspect: (Float) -> Unit,
                                   onStatus: (Int) -> Unit, modifier: Modifier = Modifier) {
    val state by localPreview.state.collectAsState()
    val aspect = state.format?.aspect(displayDegrees) ?: 9f / 16f
    val status = if (state.failed) R.string.stream_preview_failed else if (state.active) R.string.stream_live else R.string.stream_connecting
    LaunchedEffect(aspect, status) { onAspect(aspect); onStatus(status) }
    val owner = LocalLifecycleOwner.current
    var visible by remember(owner) { mutableStateOf(owner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    DisposableEffect(owner, localPreview) {
        localPreview.setVisible(visible)
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_START) { localPreview.setVisible(true); visible = true }
            if (event == Lifecycle.Event.ON_STOP) { localPreview.setVisible(false); visible = false }
        }
        owner.lifecycle.addObserver(observer)
        // onRelease отсоединяет только свой target; поздний dispose плитки не должен выключить новый preview.
        onDispose { owner.lifecycle.removeObserver(observer) }
    }
    if (visible) key(localPreview) {
        AndroidView(factory = { CameraPreviewView(it, localPreview) }, modifier = modifier,
            update = { it.update(state.format, displayDegrees) }, onRelease = { it.close() })
    }
}

/** DisplayListener замечает также 180°, когда Configuration и размеры окна не меняются. */
@Composable
internal fun StreamDisplayDegrees(): Int {
    val context = LocalContext.current
    val configuration = LocalConfiguration.current
    val activity = remember(context) {
        generateSequence(context) { (it as? ContextWrapper)?.baseContext }.filterIsInstance<Activity>().firstOrNull()
    }
    val display = activity?.windowManager?.defaultDisplay
    var degrees by remember(display) { mutableStateOf((display?.rotation ?: 0) * 90) }
    DisposableEffect(context, display, configuration) {
        degrees = (display?.rotation ?: 0) * 90
        val manager = context.getSystemService(DisplayManager::class.java)
        val listener = object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(id: Int) {}
            override fun onDisplayRemoved(id: Int) {}
            override fun onDisplayChanged(id: Int) {
                if (id == display?.displayId) degrees = display.rotation * 90
            }
        }
        manager.registerDisplayListener(listener, Handler(Looper.getMainLooper()))
        onDispose { manager.unregisterDisplayListener(listener) }
    }
    return degrees
}

/** TextureView отдаёт texture владельцам Camera2; destroy не освобождает используемый камерой output. */
internal class CameraPreviewView(context: Context, private val preview: StreamPreview, keepAwake: Boolean = true) : TextureView(context) {
    private val resources = StreamPreviewViewResources(preview)
    private var target: StreamPreviewTarget? = null
    private var format: StreamPreviewFormat? = null
    private var displayDegrees = 0
    private var closed = false
    private var frames = 0L
    private var reportFrames = 0L
    private var reportNs = 0L
    private var observedFormat: StreamPreviewFormat? = null

    init {
        keepScreenOn = keepAwake
        surfaceTextureListener = object : SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                target = resources.attach(texture)
                if (closed) return
                frames = 0; reportFrames = 0; reportNs = System.nanoTime(); observedFormat = null
                transform()
            }
            override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) { transform() }
            override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                if (target?.texture === texture) target = null
                return resources.destroy(texture) // Camera session удерживает свою ссылку до onClosed/onConfigureFailed.
            }
            override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {
                val timestamp = texture.timestamp
                if (timestamp == 0L) return // Очистка TextureView до первого кадра камеры.
                if (format != observedFormat) {
                    val native = FloatArray(16); texture.getTransformMatrix(native)
                    LogManager.addLog("INFO", "video", "direct preview native transform sensor=${format?.camera?.sensor} " +
                        "front=${format?.camera?.front} native=${listOf(native[0], native[1], native[4], native[5])}")
                    observedFormat = format
                }
                frames++
                val now = System.nanoTime()
                if (frames == 1L || now - reportNs >= 1_000_000_000) {
                    val fps = (frames - reportFrames) * 1_000_000_000.0 / (now - reportNs).coerceAtLeast(1)
                    val age = if (format?.realtimeTimestamp == true) (SystemClock.elapsedRealtimeNanos() - timestamp) / 1000 else null
                    LogManager.addLog(if (frames == 1L) "INFO" else "DEBUG", "video", "direct preview frame " +
                        "target=${target?.let(System::identityHashCode)} frames=$frames fps=${"%.1f".format(fps)} " +
                        "ageUs=$age timestampNs=$timestamp realtime=${format?.realtimeTimestamp} " +
                        "sensor=${format?.camera?.sensor} display=$displayDegrees textureRotation=${format?.textureRotation(displayDegrees)}")
                    reportNs = now; reportFrames = frames
                }
            }
        }
    }

    fun update(next: StreamPreviewFormat?, display: Int) {
        if (next == format && display == displayDegrees) return
        format = next; displayDegrees = display; transform()
    }

    private fun transform() {
        val next = format ?: return
        if (width == 0 || height == 0) return
        val destination = next.corners(displayDegrees)
        for (i in destination.indices) destination[i] *= if (i % 2 == 0) width.toFloat() else height.toFloat()
        val source = floatArrayOf(0f, 0f, width.toFloat(), 0f, width.toFloat(), height.toFloat(), 0f, height.toFloat())
        val matrix = Matrix()
        if (!matrix.setPolyToPoly(source, 0, destination, 0, 4)) {
            LogManager.addLog("ERROR", "video", "direct preview transform failed view=${width}x$height format=$next"); return
        }
        setTransform(matrix)
        LogManager.videoTrace("direct preview transform view=${width}x$height buffer=${next.width}x${next.height} " +
            "sensor=${next.camera.sensor} display=$displayDegrees textureRotation=${next.textureRotation(displayDegrees)} front=${next.camera.front}")
    }

    fun close() { closed = true; keepScreenOn = false; resources.close() }
}
