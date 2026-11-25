package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.os.Build
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.BarChart
import androidx.compose.material.icons.filled.Headset
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.PhoneInTalk
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material.icons.filled.VideocamOff
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.unit.toSize
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.utun.chat.data.AudioRoute
import com.utun.chat.data.LogManager
import com.utun.chat.viewmodel.ChatViewModel
import com.utun.chat.data.CallPhase
import com.utun.chat.data.CallController
import com.utun.chat.data.CallVideoSettings
import com.utun.chat.data.CallVideoEngine
import kotlinx.coroutines.delay

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

private fun fmtDuration(sec: Int): String {
    val m = sec / 60
    val s = sec % 60
    return "%02d:%02d".format(m, s)
}

@Composable
private fun callStatusText(phase: CallPhase, reason: Int = -1): String = when (phase) {
    CallPhase.OUTGOING -> stringResource(R.string.call_connecting)
    CallPhase.RINGING  -> stringResource(R.string.call_ringing)
    CallPhase.WAIT_DECISION -> stringResource(R.string.phone_busy_warning)
    CallPhase.ACTIVE   -> stringResource(R.string.call_talking)
    CallPhase.ENDED    -> stringResource(if (reason == 8) R.string.phone_busy_warning else R.string.call_ended)
    else               -> ""
}

@Composable
private fun CallControlButton(
    icon: ImageVector,
    contentDescription: String,
    label: String,
    active: Boolean,
    activeColor: Color = Color(0xFF1F6FEB),
    showLabel: Boolean = true,
    enabled: Boolean = true,
    onClick: () -> Unit
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            modifier = Modifier.size(60.dp)
                .background(if (active) activeColor else Color(0xFF24323D), CircleShape),
            contentAlignment = Alignment.Center
        ) {
            IconButton(onClick = onClick, enabled = enabled) {
                Icon(icon, contentDescription, tint = Color.White.copy(alpha = if (enabled) 1f else 0.38f), modifier = Modifier.size(28.dp))
            }
        }
        if (showLabel) {
            Spacer(Modifier.height(6.dp))
            Text(label, color = if (active) Color.White else Color(0xFFAEBCC8), fontSize = 11.sp)
        }
    }
}

@Composable
private fun CallVideoButton(video: CallVideoEngine, showLabel: Boolean, onOptions: () -> Unit) {
    val state by video.state.collectAsState()
    CallControlButton(
        icon = if (state.settings.enabled) Icons.Filled.Videocam else Icons.Filled.VideocamOff,
        contentDescription = stringResource(if (state.settings.enabled) R.string.call_video_disable else R.string.call_video_enable),
        label = stringResource(R.string.camera),
        active = state.settings.enabled,
        showLabel = showLabel,
        onClick = {
            onOptions()
            if (state.settings.enabled) CallController.setVideo(state.settings.copy(enabled = false))
        }
    )
}

/** Полноэкранный звонок: сигналинг/аудио/видео принадлежат контроллеру и сервису, minimize не завершает звонок.
 * При ACTIVE proximity блокирует ввод только у разговорного динамика без видео; видеопанели сохраняют TextureView при смене геометрии.
 * Ориентация окна фиксируется на время экрана и восстанавливается при выходе. */
@Composable
@OptIn(ExperimentalLayoutApi::class)
fun CallScreen(
    viewModel: ChatViewModel,
    onClose: () -> Unit,
    onMinimize: () -> Unit
) {
    val st = viewModel.callState.collectAsState().value
    val publisher = com.utun.chat.data.StreamController.state.collectAsState().value
    val viewing = com.utun.chat.data.StreamPlayerEngine.viewing.collectAsState().value
    val muted by viewModel.muted.collectAsState()
    val route by viewModel.audioRoute.collectAsState()
    val headsetAvailable by viewModel.headsetAvailable.collectAsState()
    val earpieceAvailable by viewModel.earpieceAvailable.collectAsState()
    val proximityNear by viewModel.proximityNear.collectAsState()

    val phase = st?.phase ?: CallPhase.NONE
    val duration = st?.durationSeconds ?: 0
    val name = st?.peerName?.ifEmpty { st.peerAddress ?: st.channelId } ?: ""
    val video by CallController.video.collectAsState()
    var videoMenu by remember(video) { mutableStateOf<CallVideoOption?>(null) }
    val videoActive by CallController.videoActive.collectAsState()
    val remote = video?.display?.collectAsState()?.value
    val local = video?.preview?.state?.collectAsState()?.value
    val videoState = video?.state?.collectAsState()?.value
    val transmitting = phase == CallPhase.ACTIVE && videoState?.transmitting == true
    val receiving = phase == CallPhase.ACTIVE && remote != null
    val sending = phase == CallPhase.ACTIVE && local?.active == true
    var selectedVideo by rememberSaveable(st?.callId) { mutableStateOf<CallVideoSource?>(null) }
    val expandedVideo = selectedVideo?.takeIf { if (it == CallVideoSource.REMOTE) receiving else sending }
    val viewingVideo = expandedVideo != null
    val hasVideo = receiving || sending || transmitting
    val inlineVideo = hasVideo && !viewingVideo
    val controlInsets = WindowInsets.systemBarsIgnoringVisibility.union(WindowInsets.displayCutout)
    var controlsVisible by rememberSaveable(st?.callId) { mutableStateOf(true) }
    var settingsVisible by remember(st?.callId) { mutableStateOf(false) }
    var audioGraphsVisible by rememberSaveable(st?.callId) { mutableStateOf(false) }
    var videoOptionsRequested by rememberSaveable(st?.callId) { mutableStateOf(false) }
    var videoSlot by remember { mutableStateOf(Rect.Zero) }
    var rootPosition by remember { mutableStateOf(Offset.Zero) }
    var headerBottom by remember { mutableStateOf(0f) }
    val density = LocalDensity.current
    LaunchedEffect(transmitting) { if (transmitting) videoOptionsRequested = true }
    LaunchedEffect(expandedVideo) {
        controlsVisible = true
        LogManager.addLog("INFO", "call", "video presentation=${expandedVideo ?: "controls"} remote=$receiving local=$sending")
    }
    LaunchedEffect(viewingVideo, controlsVisible, settingsVisible, videoMenu) {
        if (viewingVideo && controlsVisible && !settingsVisible && videoMenu == null) { delay(5000); controlsVisible = false }
    }

    val locked = phase == CallPhase.ACTIVE && !videoActive && route == AudioRoute.EARPIECE && proximityNear == true
    LaunchedEffect(locked, phase, controlsVisible) {
        if (locked || phase != CallPhase.ACTIVE || !controlsVisible) videoMenu = null
    }

    val context = LocalContext.current
    // Launcher живёт вместе с экраном, даже когда панель кнопок автоматически скрыта поверх видео.
    var permissionVideo by remember { mutableStateOf<CallVideoEngine?>(null) }
    var pendingCamera by remember(video) { mutableStateOf<CallVideoSettings?>(null) }
    val requestScreen = rememberScreenConsent(video)
    val cameraPermissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val requestedVideo = permissionVideo
        permissionVideo = null
        LogManager.addLog(if (granted) "INFO" else "WARN", "call", "camera permission granted=$granted")
        if (granted && requestedVideo != null && CallController.video.value === requestedVideo) {
            pendingCamera?.let { CallController.setVideo(it) }
        } else if (!granted) {
            Toast.makeText(context, R.string.camera_permission_required, Toast.LENGTH_LONG).show()
        }
    }
    val view = LocalView.current
    DisposableEffect(view, videoActive) {
        val previous = view.keepScreenOn
        if (videoActive) {
            view.keepScreenOn = true
            LogManager.addLog("INFO", "call", "video screen awake; proximity overlay disabled")
        }
        onDispose {
            if (videoActive) { view.keepScreenOn = previous; LogManager.addLog("INFO", "call", "video screen wake policy restored") }
        }
    }
    // Геометрию видео меняет TextureView; Activity сохраняет ориентацию окна звонка.
    DisposableEffect(context) {
        val activity = context.findActivity(); val previous = activity?.requestedOrientation
        try {
            activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LOCKED
            LogManager.addLog("INFO", "call", "call screen orientation locked; video rotates independently")
        } catch (e: Exception) { LogManager.addLog("ERROR", "call", "Cannot lock call orientation: $e") }
        onDispose {
            if (activity != null && previous != null && activity.requestedOrientation != previous) {
                try {
                    activity.requestedOrientation = previous
                    LogManager.addLog("INFO", "call", "video screen orientation restored=$previous")
                } catch (e: Exception) { LogManager.addLog("ERROR", "call", "Cannot restore call orientation=$previous: $e") }
            }
        }
    }
    val hasBtPermission = Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
        ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) ==
        PackageManager.PERMISSION_GRANTED
    var bluetoothCallId by rememberSaveable { mutableStateOf<Long?>(null) }
    val btPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        val requestedCall = bluetoothCallId; bluetoothCallId = null
        LogManager.addLog(if (granted) "INFO" else "WARN", "call", "Bluetooth permission=$granted call=$requestedCall")
        if (granted && requestedCall != null && st?.callId == requestedCall && phase == CallPhase.ACTIVE)
            viewModel.setAudioRoute(AudioRoute.HEADSET)
    }

    /* Пока телефон у уха (proximity) — глотаем Back, чтобы не свернуть звонок. */
    BackHandler(enabled = locked) {}
    BackHandler(enabled = viewingVideo && !locked && !settingsVisible) { selectedVideo = null }

    /* При блокировке скрываем системные панели (иначе статус-бар светится поверх оверлея). */
    val hideBars = locked || (viewingVideo && !controlsVisible)
    if (hideBars) ViewerFullscreenWindow("call id=${st?.callId}")

    Box(
        modifier = Modifier.fillMaxSize().background(Color(0xFF0F1B22)).onGloballyPositioned { rootPosition = it.positionInRoot() }
            .pointerInput(videoMenu) { if (videoMenu != null) detectTapGestures { videoMenu = null } },
        contentAlignment = Alignment.Center
    ) {
        if (phase == CallPhase.ACTIVE) video?.let {
            CallVideoStage(it, expandedVideo, videoSlot, headerBottom, onSelect = { source ->
                if (videoMenu != null) videoMenu = null
                else { videoOptionsRequested = true; selectedVideo = source }
            },
                onTap = { if (videoMenu != null) videoMenu = null else controlsVisible = !controlsVisible },
                modifier = Modifier.matchParentSize())
        }
        if (!viewingVideo || controlsVisible) {
        Column(
            modifier = Modifier.then(if (viewingVideo) Modifier.fillMaxWidth().align(Alignment.BottomCenter)
                    .background(Color(0xFF0F1B22).copy(alpha = 0.75f)) else Modifier.fillMaxSize()
                    .padding(top = with(density) { headerBottom.toDp() }))
                .windowInsetsPadding(controlInsets.only(WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom))
                .then(if (inlineVideo) Modifier else Modifier.verticalScroll(rememberScrollState()))
                .padding(vertical = if (inlineVideo) 0.dp else 24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            // Кнопки занимают свою высоту, видео получает всё оставшееся место без прокрутки.
            if (inlineVideo) Box(Modifier.weight(1f).fillMaxWidth()
                .onGloballyPositioned { videoSlot = Rect(it.positionInRoot() - rootPosition, it.size.toSize()) })
            Column(Modifier.fillMaxWidth()
                .then(if (inlineVideo) Modifier.verticalScroll(rememberScrollState()).padding(bottom = 8.dp) else Modifier),
                horizontalAlignment = Alignment.CenterHorizontally) {
            if (!viewingVideo) {
            if (!hasVideo) Box(Modifier.fillMaxWidth().height(96.dp),
                contentAlignment = Alignment.Center) {
                Box(Modifier.size(96.dp).background(Color(0xFF1F6FEB), CircleShape), contentAlignment = Alignment.Center) {
                    Text(if (name.isEmpty()) "" else String(Character.toChars(name.codePointAt(0))).uppercase(),
                        fontSize = 40.sp, color = Color.White, fontWeight = FontWeight.Bold)
                }
            }
            Spacer(Modifier.height(16.dp))
            Text(
                if (st?.preparing == true) stringResource(R.string.call_preparing_audio)
                else if (phase == CallPhase.ACTIVE) fmtDuration(duration) else callStatusText(phase, st?.reason ?: -1),
                color = Color(0xFFAEBCC8),
                fontSize = 16.sp
            )
            Spacer(Modifier.height(8.dp))
            if (st?.ongoing == true && st.phase != CallPhase.ACTIVE && !st.preparing) {
                if (publisher?.ongoing == true) Text(stringResource(R.string.phone_call_stop_publish), color = Color(0xFFFFCC80))
                if (viewing) Text(stringResource(R.string.phone_call_stop_view), color = Color(0xFFFFCC80))
            }
            ProvideTextStyle(MaterialTheme.typography.bodySmall.copy(
                color = Color(0xFFAEBCC8), fontSize = 12.sp, lineHeight = 14.sp,
                textAlign = TextAlign.Center, platformStyle = PlatformTextStyle(includeFontPadding = false)
            )) {
                Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                    horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    LaunchedEffect(Unit) {
                        LogManager.addLog("DEBUG", "call", "compact call diagnostics shown: font=12sp lineHeight=14sp gap=2dp")
                    }
                    Text(stringResource(R.string.connection_label, st?.connectionType?.label ?: stringResource(R.string.connection_none)),
                        modifier = Modifier.fillMaxWidth())
                    val bgpPath = st?.bgpPath ?: ""
                    if (bgpPath.isNotEmpty()) Text(bgpPath, modifier = Modifier.fillMaxWidth())
                    if (phase == CallPhase.ACTIVE) st?.stats?.let { stats ->
                        Text(stringResource(R.string.call_stats, stats.bufferMs, stats.reserveMs,
                            stats.minMs, stats.maxMs, "%.2f".format(stats.tempoX100 / 100.0), stats.rttMs, stats.dropped, stats.underruns),
                            modifier = Modifier.fillMaxWidth())
                        Text(stringResource(R.string.call_output_stats,
                            stats.output.queueMs.takeIf { it >= 0 }?.toString() ?: "—",
                            stats.output.outputMs.takeIf { it >= 0 }?.toString() ?: "—"),
                            modifier = Modifier.fillMaxWidth())
                    }
                }
            }
            videoState?.notice?.takeIf { !videoOptionsRequested && !transmitting }?.let {
                Text(stringResource(it), color = Color(0xFFFFB4AB), fontSize = 13.sp,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp), textAlign = TextAlign.Center)
            }
            Spacer(Modifier.height(if (hasVideo) 12.dp else 40.dp))
            }

            if (phase == CallPhase.ENDED) {
                Button(onClick = { viewModel.dismissCall(); onClose() }) {
                    Text(stringResource(R.string.close))
                }
            } else {
                if (phase == CallPhase.WAIT_DECISION) {
                    Button(onClick = { st?.let { com.utun.chat.data.CallController.continueCall(it.callId) } }) {
                        Text(stringResource(R.string.call_anyway))
                    }
                    Spacer(Modifier.height(20.dp))
                }
                if (phase == CallPhase.ACTIVE) {
                    Row(horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                        if (earpieceAvailable) {
                            CallControlButton(
                                icon = Icons.Filled.PhoneInTalk,
                                contentDescription = stringResource(R.string.earpiece),
                                label = stringResource(R.string.earpiece),
                                active = route == AudioRoute.EARPIECE,
                                showLabel = !viewingVideo,
                                onClick = { viewModel.setAudioRoute(AudioRoute.EARPIECE) }
                            )
                        }
                        CallControlButton(
                            icon = Icons.AutoMirrored.Filled.VolumeUp,
                            contentDescription = stringResource(R.string.speakerphone),
                            label = stringResource(R.string.speaker_short),
                            active = route == AudioRoute.SPEAKER,
                            showLabel = !viewingVideo,
                            onClick = { viewModel.setAudioRoute(AudioRoute.SPEAKER) }
                        )
                        if (headsetAvailable) {
                            CallControlButton(
                                icon = Icons.Filled.Headset,
                                contentDescription = stringResource(R.string.headset),
                                label = stringResource(R.string.headset),
                                active = route == AudioRoute.HEADSET,
                                showLabel = !viewingVideo,
                                onClick = {
                                    if (hasBtPermission) {
                                        viewModel.setAudioRoute(AudioRoute.HEADSET)
                                    } else {
                                        bluetoothCallId = st?.callId
                                        try { btPermissionLauncher.launch(Manifest.permission.BLUETOOTH_CONNECT) } catch (e: Exception) {
                                            bluetoothCallId = null
                                            LogManager.addLog("ERROR", "call", "Cannot request Bluetooth permission: $e")
                                            Toast.makeText(context, R.string.media_audio_failed, Toast.LENGTH_LONG).show()
                                        }
                                    }
                                }
                            )
                        }
                    }
                    Spacer(Modifier.height(28.dp))
                }

                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    if (phase == CallPhase.ACTIVE) {
                        CallControlButton(
                            icon = if (muted) Icons.Filled.MicOff else Icons.Filled.Mic,
                            contentDescription = stringResource(if (muted) R.string.microphone_enable else R.string.microphone_disable),
                            label = stringResource(if (muted) R.string.microphone_on_short else R.string.microphone_mute_short),
                            active = muted,
                            showLabel = !viewingVideo,
                            activeColor = Color(0xFFB32632),
                            onClick = { viewModel.toggleMute() }
                        )
                    }
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Box(
                            modifier = Modifier.size(72.dp).background(Color(0xFFB32632), CircleShape),
                            contentAlignment = Alignment.Center
                        ) {
                            IconButton(onClick = { viewModel.hangupCall() }) {
                                Icon(Icons.Filled.CallEnd, stringResource(if (phase == CallPhase.WAIT_DECISION) R.string.finish_call else R.string.hang_up),
                                    tint = Color.White, modifier = Modifier.size(36.dp))
                            }
                        }
                        if (!viewingVideo) {
                            Spacer(Modifier.height(6.dp))
                            Text(stringResource(if (phase == CallPhase.WAIT_DECISION) R.string.finish_call else R.string.hang_up),
                                color = Color(0xFFAEBCC8), fontSize = 12.sp)
                        }
                    }
                    if (phase == CallPhase.ACTIVE) {
                        video?.let { engine ->
                            CallVideoButton(engine, showLabel = !viewingVideo, onOptions = {
                                videoOptionsRequested = true; controlsVisible = true
                                if (!engine.state.value.settings.enabled) videoMenu = CallVideoOption.CAMERA
                            })
                        }
                    }
                }
            }
            }
        }
        Column(Modifier.align(Alignment.TopCenter).fillMaxWidth()
            .background(Color(0xFF0F1B22).copy(alpha = if (viewingVideo) 0.85f else 1f))
            .windowInsetsPadding(controlInsets.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal))
            .onGloballyPositioned { headerBottom = it.boundsInRoot().bottom - rootPosition.y }) {
            Row(Modifier.fillMaxWidth().height(56.dp).padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = { if (viewingVideo) selectedVideo = null else onMinimize() }) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack,
                        stringResource(if (viewingVideo) R.string.call_controls else R.string.minimize_call), tint = Color.White)
                }
                Text(name, modifier = Modifier.weight(1f).padding(start = 8.dp), color = Color.White,
                    fontSize = 18.sp, fontWeight = FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (phase == CallPhase.ACTIVE && !locked) IconButton(onClick = { audioGraphsVisible = true }) {
                    Icon(Icons.Filled.BarChart, stringResource(R.string.audio_graphs), tint = Color.White)
                }
            }
            if (phase == CallPhase.ACTIVE && (videoOptionsRequested || transmitting) && !locked) video?.let { engine ->
                CallVideoToolbar(engine, videoMenu, onMenuChange = { videoMenu = it },
                    onSource = { settings ->
                        if (settings.screen) requestScreen { grant ->
                            if (CallController.video.value === engine) CallController.setVideo(settings, grant)
                        } else if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED)
                            CallController.setVideo(settings)
                        else {
                            permissionVideo = engine; pendingCamera = settings
                            try { cameraPermissionLauncher.launch(Manifest.permission.CAMERA) } catch (e: Exception) {
                                permissionVideo = null; pendingCamera = null
                                LogManager.addLog("ERROR", "call", "Cannot request call camera permission: $e")
                                Toast.makeText(context, R.string.camera_unavailable, Toast.LENGTH_LONG).show()
                            }
                        }
                    }, onInteraction = { settingsVisible = it }, modifier = Modifier.fillMaxWidth()
                    .padding(start = 16.dp, end = 16.dp, bottom = 8.dp))
            }
        }
        }

        if (audioGraphsVisible && !locked && phase == CallPhase.ACTIVE) st?.let { call ->
            CallAudioGraphsDialog(call.callId) { audioGraphsVisible = false }
        }
        if (locked) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color(0xFF0F1B22))
                    .pointerInput(Unit) {
                        awaitPointerEventScope {
                            while (true) awaitPointerEvent()
                        }
                    },
                contentAlignment = Alignment.Center
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Icon(Icons.Filled.Lock, stringResource(R.string.screen_locked), tint = Color.White, modifier = Modifier.size(48.dp))
                    Spacer(Modifier.height(16.dp))
                    Text(stringResource(R.string.screen_locked), color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Medium)
                    Spacer(Modifier.height(8.dp))
                    Text(stringResource(R.string.move_phone_away), color = Color(0xFFAEBCC8), fontSize = 14.sp)
                }
            }
        }
    }
}
