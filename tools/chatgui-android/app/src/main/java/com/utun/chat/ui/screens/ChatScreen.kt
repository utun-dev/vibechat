package com.utun.chat.ui.screens

import com.utun.chat.data.NativeLib

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import android.os.SystemClock
import android.widget.Toast
import android.view.ViewConfiguration
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Equalizer
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.Link
import androidx.compose.material.icons.filled.People
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.toggleableState
import androidx.compose.ui.state.ToggleableState
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.utun.chat.ChatApplication
import com.utun.chat.data.Channel
import com.utun.chat.data.Message
import com.utun.chat.data.RadioAudioEngine
import com.utun.chat.data.RadioAutoSettings
import com.utun.chat.data.LogManager
import com.utun.chat.data.RadioOverlayService
import com.utun.chat.ui.components.ConversationMessages
import com.utun.chat.ui.components.CreatePollDialog
import com.utun.chat.ui.components.ConversationComposer
import com.utun.chat.ui.components.PortableRadioIcon
import com.utun.chat.ui.components.GroupAvatar
import com.utun.chat.ui.components.GroupPropertiesDialog
import com.utun.chat.ui.components.OwnStreamButton
import com.utun.chat.ui.components.MediaAudioDeviceButton
import com.utun.chat.data.ConversationTarget
import com.utun.chat.viewmodel.ChatViewModel
import com.utun.chat.data.StreamSource
import androidx.compose.material.icons.filled.LiveTv
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.CancellationException

/** Канальный чат: история/ответы/правки принадлежат ViewModel, draft и выбор вложения — общему composer.
 * Кнопка PTT удерживает источник BUTTON до отпускания/dispose; двойной короткий тап фиксирует передачу.
 * Просмотр трансляций исключает собственный источник. SAF export не изменяет оригинал и сообщает об отказе записи. */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun ChatScreen(
    channel: Channel,
    messages: List<Message>,
    connected: Boolean,
    viewModel: ChatViewModel,
    onMembersClick: () -> Unit,
    onFiles: (String) -> Unit = {},
    onInvite: () -> Unit,
    onBack: () -> Unit,
    onPlayVideo: (Message) -> Unit = {},
    onViewPhoto: (Message) -> Unit = {},
    onRecordVideo: () -> Unit = {},
    onCapturePhoto: () -> Unit = {},
    onStreams: () -> Unit = {},
    onOwnStream: (String) -> Unit = {},
    onWatchStream: (StreamSource) -> Unit = {},
    onWatchAllStreams: () -> Unit = {}
) {
    val listState = key(channel.id) { rememberLazyListState() }
    val context = LocalContext.current
    val view = LocalView.current
    val pttCenterOffset = with(LocalDensity.current) { 60.dp.toPx() }
    val scope = rememberCoroutineScope()
    var properties by remember(channel.id) { mutableStateOf(false) }
    var menu by remember(channel.id) { mutableStateOf(false) }
    val streams by viewModel.streamSources.collectAsState()
    val watchSources = streams.filterNot { it.self }
    var chooseStream by remember(channel.id) { mutableStateOf(false) }
    LaunchedEffect(channel.id, watchSources.size, streams.size) {
        if (watchSources.isEmpty()) chooseStream = false
        LogManager.addLog("INFO", "stream", "watch stream header channel=${channel.id} remote=${watchSources.size} " +
            "ownExcluded=${streams.size - watchSources.size} visible=${watchSources.isNotEmpty()}")
    }
    if (chooseStream) AlertDialog(
        onDismissRequest = { chooseStream = false },
        title = { Text(stringResource(R.string.stream_choose_source)) },
        text = {
            Column(Modifier.heightIn(max = 360.dp).verticalScroll(rememberScrollState())) {
                TextButton(onClick = { chooseStream = false; onWatchAllStreams() }) {
                    Text(stringResource(R.string.stream_watch_all))
                }
                watchSources.forEach { source ->
                    TextButton(onClick = { chooseStream = false; onWatchStream(source) }) {
                        Text("${source.displayName} · ${source.id.toULong()}")
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = { chooseStream = false }) { Text(stringResource(R.string.cancel)) } }
    )

    /* рация */
    val radioActive by viewModel.radioActive.collectAsState()
    val radioTalking by viewModel.radioTalking.collectAsState()
    val overlayShown by RadioOverlayService.isShown.collectAsState()
    val localTalking by RadioAudioEngine.instance().talkingFlow.collectAsState()
    val captureReady by RadioAudioEngine.instance().captureReady.collectAsState()

    /* звук сообщений канала */
    val soundOn by viewModel.soundOn.collectAsState()
    val reactionCatalog by viewModel.reactionCatalog.collectAsState()
    val interactionFailed by viewModel.interactionFailed.collectAsState()
    var createPoll by rememberSaveable(channel.id) { mutableStateOf(false) }
    if (createPoll) CreatePollDialog({ createPoll = false }, viewModel::createPoll)
    if (interactionFailed) AlertDialog(onDismissRequest = viewModel::dismissInteractionError,
        text = { Text(stringResource(R.string.poll_interaction_failed)) },
        confirmButton = { TextButton(onClick = viewModel::dismissInteractionError) { Text(stringResource(R.string.close)) } })

    /* save dialog for downloaded files */
    var pendingSaveFilePath by rememberSaveable { mutableStateOf<String?>(null) }
    var saveError by remember(channel.id) { mutableStateOf(false) }
    val saveFileLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { saveUri ->
        val filePath = pendingSaveFilePath
        pendingSaveFilePath = null
        if (saveUri != null && filePath != null) scope.launch {
            try { saveError = !withContext(Dispatchers.IO) { viewModel.saveDownloadedFile(filePath, saveUri) } }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                saveError = true
                LogManager.addLog("ERROR", "Attachment", "File export failed: $e")
            }
        }
    }

    val playbackState by viewModel.playbackState.collectAsState()
    val originals by viewModel.replyOriginals.collectAsState()
    val anchor by viewModel.historyAnchor.collectAsState()
    val jump by viewModel.messageJump.collectAsState()
    val originalMissing by viewModel.originalMissing.collectAsState()
    var highlighted by remember(channel.id) { mutableStateOf("") }
    if (originalMissing) AlertDialog(onDismissRequest = viewModel::dismissOriginalMissing,
        text = { Text(stringResource(R.string.reply_original_missing)) },
        confirmButton = { TextButton(onClick = viewModel::dismissOriginalMissing) { Text(stringResource(R.string.close)) } })

    LaunchedEffect(jump) {
        val destination = jump ?: return@LaunchedEffect
        val index = if (destination.messageId.isEmpty()) 0
            else messages.asReversed().indexOfFirst { it.messageId == destination.messageId }.let { if (it < 0) -1 else it + 1 }
        if (index >= 0) {
            listState.scrollToItem(index)
            highlighted = destination.messageId
            kotlinx.coroutines.delay(1500)
            highlighted = ""
        }
    }

    /* своё сообщение — подтягиваемся вниз (в reverseLayout индекс 0 — это низ) */
    LaunchedEffect(messages.size) {
        if (anchor.isEmpty() && messages.lastOrNull()?.isOutgoing == true) {
            listState.scrollToItem(0)
        }
    }

    Column(
        modifier = Modifier.fillMaxSize().imePadding()
    ) {
        TopAppBar(
            title = {
                BoxWithConstraints {
                    val showAvatar = maxWidth >= 96.dp
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (showAvatar) GroupAvatar(channel.id, channel.name, size = 32.dp)
                        Text(channel.name, maxLines = 1, overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis)
                    }
                }
            },
            navigationIcon = {
                IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back)) }
            },
            actions = {
                if (watchSources.isNotEmpty()) Box(
                    modifier = Modifier.size(48.dp).clip(CircleShape).combinedClickable(
                        role = Role.Button,
                        onClickLabel = stringResource(R.string.stream_watch_all),
                        onLongClickLabel = stringResource(R.string.stream_choose_source),
                        onClick = {
                            LogManager.addLog("INFO", "stream", "watch stream header short click channel=${channel.id} " +
                                "action=watchAll remote=${watchSources.size} ownExcluded=${streams.size - watchSources.size}")
                            onWatchAllStreams()
                        },
                        onLongClick = {
                            LogManager.addLog("INFO", "stream", "watch stream header long click channel=${channel.id} " +
                                "action=chooseStream remote=${watchSources.size} ownExcluded=${streams.size - watchSources.size}")
                            chooseStream = true
                        }
                    ),
                    contentAlignment = Alignment.Center
                ) {
                    BadgedBox(badge = { if (watchSources.size > 1) Badge { Text(watchSources.size.toString()) } }) {
                        Icon(Icons.Filled.LiveTv, stringResource(R.string.stream_available, watchSources.size), tint = Color(0xFF4CAF50))
                    }
                }
                OwnStreamButton("channel=${channel.id}", onOwnStream)
                IconButton(
                    onClick = { viewModel.toggleRadio() },
                    modifier = Modifier.background(
                        if (radioActive) Color(0xFF27AE60) else Color.Transparent, CircleShape)
                ) {
                    val action = stringResource(if (radioActive) R.string.radio_turn_off else R.string.radio_turn_on)
                    PortableRadioIcon(stringResource(R.string.radio_toggle, action))
                }
                IconButton(onClick = { properties = true }) { Icon(Icons.Filled.Settings, stringResource(R.string.group_properties)) }
                IconButton(onClick = onMembersClick) { Icon(Icons.Filled.People, stringResource(R.string.members)) }
                Box {
                    IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, stringResource(R.string.options)) }
                    DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        DropdownMenuItem(text = { Text(stringResource(R.string.stream_title)) }, onClick = { menu = false; onStreams() })
                        DropdownMenuItem(text = { Text(stringResource(R.string.files_title)) },
                            onClick = { menu = false; onFiles("") })
                        DropdownMenuItem(text = { Text(stringResource(R.string.files_mine)) },
                            onClick = { menu = false; onFiles(NativeLib.getMySigningAddress()) })
                        DropdownMenuItem(text = { Text(stringResource(R.string.poll_create)) }, onClick = { menu = false; createPoll = true })
                        DropdownMenuItem(text = { Text(stringResource(R.string.invite)) },
                            leadingIcon = { Icon(Icons.Filled.Link, null) }, onClick = { menu = false; onInvite() })
                        DropdownMenuItem(text = { Text(stringResource(R.string.sound)) },
                            trailingIcon = { Text(if (soundOn) "\uD83D\uDD14" else "\uD83D\uDD15") },
                            onClick = { menu = false; viewModel.toggleSound() })
                    }
                }
            }
        )
        if (properties) GroupPropertiesDialog(channel, onDismiss = {
            properties = false
            if (soundOn == com.utun.chat.data.NotificationSound.isMuted(channel.id)) viewModel.toggleSound()
        })
        if (radioActive) {
            RadioSourceBar(viewModel)
        }
        if (saveError) Text(stringResource(R.string.file_save_failed), color = MaterialTheme.colorScheme.error,
            modifier = Modifier.padding(8.dp))
        if (radioTalking != null) {
            Surface(color = MaterialTheme.colorScheme.primaryContainer) {
                Text(
                    text = stringResource(R.string.radio_speaking, viewModel.radioName(radioTalking!!)),
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
                    style = MaterialTheme.typography.bodyMedium
                )
            }
        }
        Box(modifier = Modifier.weight(1f).fillMaxWidth().onGloballyPositioned { coordinates ->
            val windowPosition = coordinates.positionInWindow()
            val screenOrigin = IntArray(2)
            val windowOrigin = IntArray(2)
            view.getLocationOnScreen(screenOrigin)
            view.getLocationInWindow(windowOrigin)
            RadioOverlayService.setDefaultCenter(
                (windowPosition.x + coordinates.size.width / 2f + screenOrigin[0] - windowOrigin[0]).toInt(),
                (windowPosition.y + pttCenterOffset + screenOrigin[1] - windowOrigin[1]).toInt())
        }) {
            ConversationMessages(
                target = ConversationTarget.Channel(channel.id), messages = messages, state = listState,
                playbackState = playbackState, modifier = Modifier.fillMaxSize(),
                onPlayVoice = { path, duration -> viewModel.playVoiceMessage(path, duration) },
                onSeekVoice = { path, position, duration -> viewModel.seekVoice(path, position, duration) },
                onDownloadFile = { viewModel.downloadAttachment(it) },
                onOpenFile = {
                    if (it.filePath.isNotEmpty() && pendingSaveFilePath == null) {
                        pendingSaveFilePath = it.filePath; saveError = false
                        try { saveFileLauncher.launch(it.text) } catch (e: Exception) {
                            pendingSaveFilePath = null; saveError = true
                            LogManager.addLog("ERROR", "Attachment", "Cannot launch file export: $e")
                        }
                    }
                },
                onOpenImage = onViewPhoto, onPlayVideo = onPlayVideo, onInviteLink = viewModel::openLink, groupName = channel.name, groupEd = channel.groupEd,
                reactionCatalog = reactionCatalog, onVote = viewModel::vote, onReact = viewModel::react, authorName = viewModel::pollAuthorName,
                onReply = { viewModel.selectReply(it) }, onQuote = viewModel::selectReply,
                onOriginal = viewModel::showOriginal, originals = originals, highlightedId = highlighted,
                onEdit = viewModel::selectEdit
            )
            if (anchor.isNotEmpty()) FilledTonalButton(onClick = viewModel::showLatestMessages,
                modifier = Modifier.align(Alignment.BottomEnd).padding(12.dp)) { Text(stringResource(R.string.reply_latest)) }
            if (radioActive && !overlayShown) {
                RadioPttButton(
                    modifier = Modifier.align(Alignment.TopCenter).padding(top = 12.dp),
                    isTalking = localTalking,
                    captureReady = captureReady,
                    onTalkBegin = { viewModel.radioTalkBegin() },
                    onTalkEnd = { viewModel.radioTalkEnd() }
                )
            }
        }
        ConversationComposer(ConversationTarget.Channel(channel.id), viewModel, onRecordVideo, onCapturePhoto)
    }
}

/* Передача сразу при касании; двойной тап фиксирует, следующий тап завершает. */

@Composable
private fun RadioPttButton(
    modifier: Modifier = Modifier,
    isTalking: Boolean = false,
    captureReady: Boolean = false,
    onTalkBegin: () -> Boolean,
    onTalkEnd: () -> Unit
) {
    val doubleTapTimeout = ViewConfiguration.getDoubleTapTimeout().toLong().coerceAtMost(RadioAudioEngine.RELEASE_HOLD_MS)
    var latched by remember { mutableStateOf(false) }
    var lastTapUp by remember { mutableStateOf(0L) }
    LaunchedEffect(isTalking) {
        if (!isTalking) { latched = false; lastTapUp = 0L }
    }

    Surface(
        modifier = modifier
            .size(96.dp)
            .pointerInput(Unit) {
                try {
                    awaitPointerEventScope {
                        while (true) {
                            val down = awaitFirstDown(requireUnconsumed = false)
                            val downId = down.id
                            down.consume()

                            val now = SystemClock.uptimeMillis()
                            val isSecondTap = lastTapUp != 0L && (now - lastTapUp) <= doubleTapTimeout

                            val wasLatched = latched
                            var started = false
                            if (wasLatched) {
                                latched = false
                                lastTapUp = 0L
                                onTalkEnd()
                            } else started = onTalkBegin()

                            var released = false
                            try {
                                while (true) {
                                    val ev = awaitPointerEvent()
                                    val c = ev.changes.firstOrNull { it.id == downId } ?: break
                                    c.consume()
                                    if (!c.pressed) { released = true; break }
                                }
                                val upTime = SystemClock.uptimeMillis()
                                val tapped = started && released && upTime - now <= RadioAudioEngine.RELEASE_HOLD_MS
                                if (!wasLatched && tapped && isSecondTap) {
                                    latched = true
                                    lastTapUp = 0L
                                } else lastTapUp = if (!wasLatched && tapped) upTime else 0L
                            } finally {
                                if (!wasLatched && !latched) onTalkEnd()
                            }
                        }
                    }
                } finally {
                    latched = false
                    lastTapUp = 0L
                    onTalkEnd()
                }
            },
        shape = CircleShape,
        border = if (captureReady) null else BorderStroke(3.dp, Color(RadioOverlayService.COLOR_WAITING)),
        color = when {
            isTalking && !captureReady -> Color(RadioOverlayService.COLOR_WAITING)
            isTalking -> Color(0xFFFFB4AB)
            else -> Color(0xFF81C784)
        },
        shadowElevation = 6.dp
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text("\uD83C\uDF99", fontSize = 36.sp)
        }
    }
}

/* Единый выбор входа/выхода, PTT громкостью, VAD и плавающая PTT-кнопка. */
@Composable
private fun RadioSourceBar(viewModel: ChatViewModel) {
    val context = LocalContext.current
    val provider = ChatApplication.instance.configProvider
    val scope = rememberCoroutineScope()
    val overlayEnabled by provider.radioOverlayEnabled.collectAsState(initial = provider.getRadioOverlay())
    val autoSettings by provider.radioAutoSettings.collectAsState(initial = remember { provider.getRadioAutoSettings() })
    val volumePttEnabled by provider.radioVolumePttEnabled.collectAsState(initial = provider.getRadioVolumePtt())
    val overlayShown by RadioOverlayService.isShown.collectAsState()
    val overlayStatus by RadioOverlayService.status.collectAsState()

    Surface(color = Color(0xFF0F1B22)) {
        Column {
            Row(
                modifier = Modifier.fillMaxWidth().heightIn(min = 72.dp).padding(horizontal = 12.dp, vertical = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Row(
                    modifier = Modifier.weight(1f).horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    MediaAudioDeviceButton(tint = Color.White)
                }
                VerticalDivider(modifier = Modifier.height(32.dp), color = Color(0xFF344550))
                RadioControlButton(Icons.AutoMirrored.Filled.VolumeUp, stringResource(R.string.volume_ptt),
                    volumePttEnabled, volumeKeys = true) {
                    viewModel.setRadioVolumePtt(!volumePttEnabled)
                }
                RadioAutomaticButton(autoSettings) { viewModel.setRadioAutomatic(!autoSettings.enabled) }
                RadioControlButton(null, stringResource(R.string.floating_ptt), overlayShown, floating = true) {
                    scope.launch {
                        try {
                            if (overlayEnabled && !overlayShown) RadioOverlayService.requestPermissions()
                            else RadioOverlayService.setEnabled(!overlayEnabled)
                        } catch (e: CancellationException) { throw e }
                        catch (e: Exception) {
                            LogManager.addLog("ERROR", "radio", "Cannot change overlay setting enabled=${!overlayEnabled}: $e")
                            Toast.makeText(context, R.string.overlay_start_failed, Toast.LENGTH_LONG).show()
                        }
                    }
                }
            }
            if (overlayEnabled && !overlayShown) overlayStatus?.let { message ->
                Text(stringResource(message), fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 4.dp).clickable { RadioOverlayService.requestPermissions() })
            }
        }
    }
}

/* Один обработчик жестов: удержание открывает мини-блок и не переключает автопередачу. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun RadioAutomaticButton(settings: RadioAutoSettings, onClick: () -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    val label = stringResource(if (settings.enabled) R.string.radio_auto_disable else R.string.radio_auto_enable)
    val settingsLabel = stringResource(R.string.radio_auto_quick_settings)
    val modeLabel = stringResource(when (settings.mode) {
        RadioAutoSettings.LEVEL -> R.string.radio_level_mode
        RadioAutoSettings.VAD_LEVEL -> R.string.radio_vad_level_mode
        else -> R.string.vad_button
    })
    val foreground = if (settings.enabled) Color.White else Color(0xFFAEBCC8)
    Box {
        Box(
            Modifier.size(48.dp).clip(CircleShape)
                .background(if (settings.enabled) Color(0xFF1F6FEB) else Color(0xFF24323D))
                .combinedClickable(role = Role.Switch, onLongClickLabel = settingsLabel,
                    onLongClick = {
                        LogManager.addLog("DEBUG", "radio", "Open automatic transmission quick settings mode=${settings.mode}")
                        expanded = true
                    }, onClick = onClick)
                .semantics {
                    contentDescription = "$modeLabel. $label. $settingsLabel"
                    toggleableState = ToggleableState(settings.enabled)
                },
            contentAlignment = Alignment.Center
        ) {
            if (settings.mode == RadioAutoSettings.LEVEL)
                Icon(Icons.Default.Equalizer, contentDescription = null, tint = foreground, modifier = Modifier.size(28.dp))
            else Text(if (settings.mode == RadioAutoSettings.VAD_LEVEL) "VAD+" else stringResource(R.string.vad_button),
                color = foreground, fontSize = 12.sp, maxLines = 1)
        }
        DropdownMenu(expanded = expanded, onDismissRequest = {
            expanded = false
            LogManager.addLog("DEBUG", "radio", "Close automatic transmission quick settings")
        }, modifier = Modifier.width(320.dp).heightIn(max = 480.dp)) {
            Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                RadioAutomaticSettings(ChatApplication.instance.configProvider, compact = true)
            }
        }
    }
}

@Composable
private fun RadioControlButton(
    icon: ImageVector?, label: String, active: Boolean,
    centerIcon: ImageVector? = null, bluetooth: Boolean = false, floating: Boolean = false,
    volumeKeys: Boolean = false, text: String? = null, onClick: () -> Unit
) {
    IconToggleButton(
        checked = active,
        onCheckedChange = { onClick() },
        modifier = Modifier.size(48.dp),
        colors = IconButtonDefaults.iconToggleButtonColors(
            containerColor = Color(0xFF24323D), contentColor = Color(0xFFAEBCC8),
            checkedContainerColor = Color(0xFF1F6FEB), checkedContentColor = Color.White)
    ) {
        if (text != null) {
            Text(text, fontSize = 12.sp, modifier = Modifier.semantics { contentDescription = label })
            return@IconToggleButton
        }
        if (floating) {
            RadioFloatingIcon(label)
            return@IconToggleButton
        }
        if (volumeKeys) {
            RadioVolumeKeysIcon(label)
            return@IconToggleButton
        }
        val background = if (active) Color(0xFF1F6FEB) else Color(0xFF24323D)
        Box(modifier = Modifier.size(34.dp), contentAlignment = Alignment.Center) {
            Icon(requireNotNull(icon), contentDescription = label, modifier = Modifier.size(if (centerIcon != null) 32.dp else 28.dp))
            if (centerIcon != null) {
                Box(Modifier.size(20.dp).background(background, CircleShape), contentAlignment = Alignment.Center) {
                    Icon(centerIcon, contentDescription = null, modifier = Modifier.size(15.dp))
                }
            }
            if (bluetooth) {
                Box(Modifier.align(Alignment.BottomEnd).size(16.dp).background(background, CircleShape),
                    contentAlignment = Alignment.Center) {
                    Icon(Icons.Default.Bluetooth, contentDescription = null, modifier = Modifier.size(14.dp))
                }
            }
        }
    }
}

/* Динамик по центру, плюс сверху и минус снизу — аппаратные кнопки громкости. */
@Composable
private fun RadioVolumeKeysIcon(label: String) {
    val color = LocalContentColor.current
    Box(Modifier.size(34.dp).semantics { contentDescription = label }, contentAlignment = Alignment.Center) {
        Icon(Icons.AutoMirrored.Filled.VolumeUp, contentDescription = null, modifier = Modifier.size(22.dp))
        Canvas(Modifier.matchParentSize()) {
            val half = 2.5.dp.toPx()
            val top = 3.5.dp.toPx()
            val bottom = size.height - top
            val stroke = 1.7.dp.toPx()
            drawLine(color, Offset(center.x - half, top), Offset(center.x + half, top), stroke, cap = StrokeCap.Round)
            drawLine(color, Offset(center.x, top - half), Offset(center.x, top + half), stroke, cap = StrokeCap.Round)
            drawLine(color, Offset(center.x - half, bottom), Offset(center.x + half, bottom), stroke, cap = StrokeCap.Round)
        }
    }
}

/* Кружок и четыре стрелки наружу — плавающая/перемещаемая PTT-кнопка. */
@Composable
private fun RadioFloatingIcon(label: String) {
    val color = LocalContentColor.current
    Canvas(Modifier.size(28.dp).semantics { contentDescription = label }) {
        val c = center
        val radius = 4.dp.toPx()
        val stroke = 1.7.dp.toPx()
        val start = 6.dp.toPx()
        val end = 13.dp.toPx()
        val head = 3.dp.toPx()
        drawCircle(color, radius, c, style = Stroke(stroke))
        for (direction in listOf(Offset(1f, 0f), Offset(-1f, 0f), Offset(0f, 1f), Offset(0f, -1f))) {
            val tip = c + direction * end
            val perpendicular = Offset(-direction.y, direction.x)
            drawLine(color, c + direction * start, tip, strokeWidth = stroke)
            drawLine(color, tip, tip - direction * head + perpendicular * head, strokeWidth = stroke)
            drawLine(color, tip, tip - direction * head - perpendicular * head, strokeWidth = stroke)
        }
    }
}
