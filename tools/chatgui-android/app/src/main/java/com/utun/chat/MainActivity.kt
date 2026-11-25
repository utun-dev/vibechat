package com.utun.chat

import com.utun.chat.R
import com.utun.chat.data.AppLanguage
import android.Manifest
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import com.utun.chat.data.MicrophonePermission
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import com.utun.chat.data.CallController
import com.utun.chat.data.CallPhase
import com.utun.chat.data.RadioCallController
import com.utun.chat.data.RadioCallPhase
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull
import androidx.appcompat.app.AppCompatActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.statusBars
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.compose.LifecycleStartEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import com.utun.chat.data.Channel
import com.utun.chat.data.IncomingCallManager
import com.utun.chat.data.InviteData
import com.utun.chat.data.LogManager
import com.utun.chat.data.StreamController
import com.utun.chat.data.Message
import com.utun.chat.data.ConversationTarget
import com.utun.chat.data.NativeLib
import com.utun.chat.data.StartupIssue
import com.utun.chat.data.FileLocation
import com.utun.chat.data.MemberFilesProtocol
import com.utun.chat.data.RadioOverlayService
import com.utun.chat.data.RadioAudioEngine
import com.utun.chat.data.RadioPttSource
import com.utun.chat.headless.HeadlessService
import com.utun.chat.ui.theme.VibeChatTheme
import com.utun.chat.ui.components.ActiveCallBanner
import com.utun.chat.ui.screens.*
import com.utun.chat.viewmodel.ChatViewModel
import com.utun.chat.viewmodel.MemberFilesViewModel
import kotlinx.coroutines.launch
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.collect

/** Activity владеет навигацией и permission callbacks; ядро и медиа продолжают жить в Application/сервисах.
 * Снимок начальных настроек читается на IO с повтором после ошибки; foreground service запускается после setup commit. */
class MainActivity : AppCompatActivity() {
    enum class Screen { ChannelList, Chat, MemberList, Files, Settings, Logs, CameraRecord, Call, Dm, Streams }
    private data class StartupSettings(val firstLaunch: Boolean, val name: String)

    private var overLock = false
    private var inputScreen = "setup"
    private var requestedFiles by mutableStateOf<FileLocation?>(null)
    private var requestedLink by mutableStateOf<String?>(null)

    private fun readLinkIntent(value: Intent) {
        if (value.action != Intent.ACTION_VIEW) return
        val link = value.dataString ?: return
        value.data = null
        if (!link.startsWith("utun://") || link.toByteArray(Charsets.UTF_8).size > 4096 || link.any { it <= ' ' || it == '\u007f' }) {
            LogManager.addLog("WARN", "LINK", "Rejected external URL bytes=${link.length}"); return
        }
        requestedLink = link
        LogManager.addLog("INFO", "LINK", "External URL received bytes=${link.length}")
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString("pending_object_link", requestedLink)
        super.onSaveInstanceState(outState)
    }
    private var volumePttEnabled = false
    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        val started = SystemClock.uptimeMillis()
        try {
            return super.dispatchTouchEvent(event)
        } finally {
            val elapsed = SystemClock.uptimeMillis() - started
            if (elapsed >= 100) LogManager.addLog("WARN", "UI_INPUT",
                "Slow touch: screen=$inputScreen action=${event.actionMasked} x=${event.x} y=${event.y} elapsedMs=$elapsed")
        }
    }

    private var microphoneAction: (() -> Unit)? = null
    private var microphoneReason = ""
    private val overlayPermissionRequester: () -> Unit = {
        try {
            overlayPermissionLauncher.launch(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "RadioOverlay", "Cannot open overlay permission settings: $e")
            RadioOverlayService.overlayPermissionResult()
        }
    }
    private val overlayPermissionLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        RadioOverlayService.overlayPermissionResult()
    }
    private val microphoneRequester: (String, () -> Unit) -> Unit = { reason, action -> requestMicrophone(reason, action) }
    private val microphoneLauncher = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val action = microphoneAction
        microphoneAction = null
        LogManager.addLog(if (granted) "INFO" else "WARN", "PERMISSION", "Microphone result=$granted reason=$microphoneReason")
        if (granted) activityEffect("microphone granted action") { action?.invoke() }
        else {
            Toast.makeText(this, AppLanguage.text(R.string.microphone_permission_calls), Toast.LENGTH_LONG).show()
            if (!shouldShowRequestPermissionRationale(Manifest.permission.RECORD_AUDIO)) showMicrophoneSettings()
        }
    }

    private fun requestMicrophone(reason: String, action: () -> Unit) {
        if (MicrophonePermission.granted()) { action(); return }
        if (microphoneAction != null) {
            LogManager.addLog("WARN", "PERMISSION", "Microphone request already pending: $microphoneReason; ignored=$reason")
            return
        }
        microphoneReason = reason
        microphoneAction = action
        LogManager.addLog("INFO", "PERMISSION", "Requesting RECORD_AUDIO: $reason")
        try {
            microphoneLauncher.launch(Manifest.permission.RECORD_AUDIO)
        } catch (e: Exception) {
            microphoneAction = null
            LogManager.addLog("ERROR", "PERMISSION", "Cannot request microphone permission: $e")
            Toast.makeText(this, AppLanguage.text(R.string.microphone_request_failed), Toast.LENGTH_LONG).show()
        }
    }

    private fun showMicrophoneSettings() {
        activityEffect("microphone settings dialog") { android.app.AlertDialog.Builder(this)
            .setMessage(AppLanguage.text(R.string.microphone_permission_settings))
            .setPositiveButton(AppLanguage.text(R.string.settings)) { _, _ ->
                try {
                    startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "PERMISSION", "Cannot open app settings: $e")
                }
            }
            .setNegativeButton(AppLanguage.text(R.string.cancel), null).show() }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedLink = savedInstanceState?.getString("pending_object_link")
        readLinkIntent(intent)
        RadioCallController.notificationIntent(intent)
        readFilesIntent(intent)
        lifecycleScope.launch {
            val provider = ChatApplication.instance.configProvider
            provider.changes.observe("volume PTT activity", provider.radioVolumePttEnabled).collect { enabled ->
                volumePttEnabled = enabled
                if (!enabled) releaseVolumePtt("option-disabled")
            }
        }

        LogManager.addLog("INFO", "LANGUAGE",
            "UI language applied: selected=${AppLanguage.selectedTag().ifEmpty { "system" }} " +
                "effective=${resources.configuration.locales.toLanguageTags()}")
        AppLanguage.applied()

        requestNotificationPermission()

        /* Входящий звонок: showWhenLocked/turnScreenOn ДО setContent, иначе полноэкранный
           intent мигает и UI не отрисовывается поверх keyguard (холодный старт). */
        if (incomingCallId(intent) != 0L) {
            LogManager.addLog("INFO", "MAIN", "launched for incoming call 0x%016x".format(incomingCallId(intent)))
            showOverLockScreen()
        }

        /* Светлые системные значки на тёмном фоне приложения. */
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT)
        )

        setContent {
            VibeChatTheme {
                val coreFailure by AppEventHandler.startupFailure.collectAsStateWithLifecycle()
                val failure = coreFailure
                if (failure != null) {
                    AlertDialog(
                        onDismissRequest = {},
                        text = {
                            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                                Text(when (failure.issue) {
                                    StartupIssue.INVALID_KEY -> stringResource(if (failure.fromSettings) R.string.overwrite_keys_warning else R.string.startup_invalid_key)
                                    StartupIssue.DATABASE -> stringResource(R.string.startup_database_incompatible)
                                    StartupIssue.KEY_ACCESS -> stringResource(R.string.startup_key_access_failed)
                                    StartupIssue.KEY_SAVED -> stringResource(R.string.startup_key_saved)
                                    else -> failure.message
                                })
                                if (failure.canRecreate) {
                                    Text(stringResource(R.string.startup_database_recreate_warning))
                                    Text(failure.message.substringAfter('\n'))
                                }
                                failure.recoveryError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                                if (failure.resetFailed) Text(stringResource(R.string.database_reset_failed), color = MaterialTheme.colorScheme.error)
                                if (failure.recreating) LinearProgressIndicator(Modifier.fillMaxWidth())
                            }
                        },
                        confirmButton = {
                            if (failure.canRecreate) TextButton(
                                onClick = { AppEventHandler.recreateStartupDatabase { viewModelStore.clear() } }, enabled = !failure.recreating
                            ) {
                                Text(stringResource(R.string.startup_database_recreate))
                            } else TextButton(onClick = { AppEventHandler.recoverStartup() }, enabled = !failure.recreating) {
                                val label = when {
                                    failure.issue != StartupIssue.INVALID_KEY -> R.string.retry
                                    failure.fromSettings -> R.string.overwrite
                                    else -> R.string.startup_replace_key
                                }
                                Text(stringResource(label))
                            }
                        },
                        dismissButton = {
                            TextButton(onClick = { finishAndRemoveTask() }, enabled = !failure.recreating) {
                                Text(stringResource(R.string.startup_exit))
                            }
                        }
                    )
                    if (!failure.fromSettings) return@VibeChatTheme
                }
                val provider = ChatApplication.instance.configProvider
                var startup by remember { mutableStateOf<StartupSettings?>(null) }
                var startupFailed by remember { mutableStateOf(false) }
                var startupAttempt by remember { mutableStateOf(0) }
                LaunchedEffect(startupAttempt) {
                    startupFailed = false
                    try {
                        provider.changes.awaitIdle()
                        startup = withContext(Dispatchers.IO) {
                            StartupSettings(provider.isFirstLaunch(), provider.getString("node.name") ?: provider.getDeviceName())
                        }
                        LogManager.addLog("INFO", "MAIN", "Startup preferences loaded setup=${startup?.firstLaunch}")
                    } catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        LogManager.addLog("ERROR", "MAIN", "Startup preferences unavailable: $e")
                        startupFailed = true
                    }
                }
                if (startupFailed) AlertDialog(
                    onDismissRequest = {},
                    text = { Text(stringResource(R.string.settings_update_failed)) },
                    confirmButton = { TextButton(onClick = { startupAttempt++ }) { Text(stringResource(R.string.retry)) } }
                )
                val loaded = startup
                if (loaded == null) {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { LinearProgressIndicator() }
                    return@VibeChatTheme
                }
                var firstLaunch by remember { mutableStateOf(loaded.firstLaunch) }
                LaunchedEffect(firstLaunch) {
                    if (firstLaunch) {
                        LogManager.addLog("INFO", "SETUP", "Showing first launch wizard")
                    } else try {
                        if (withContext(Dispatchers.IO) { provider.getServiceEnabled() } && !HeadlessService.isActive()) {
                            checkNotNull(ContextCompat.startForegroundService(this@MainActivity,
                                Intent(this@MainActivity, HeadlessService::class.java))) { "Foreground service unavailable" }
                            LogManager.addLog("INFO", "MAIN", "Foreground service start requested")
                        }
                    } catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        LogManager.addLog("ERROR", "MAIN", "Foreground service start failed: $e")
                        startup = null; startupFailed = true
                    }
                }
                if (firstLaunch) {
                    FirstLaunchScreen(
                        initialName = loaded.name,
                        onComplete = { name ->
                            provider.changes.submit("first launch") { provider.completeFirstLaunch(name) }.await()
                            firstLaunch = false
                        }
                    )
                    return@VibeChatTheme
                }
                val vm: ChatViewModel = viewModel(factory = ChatViewModel.Factory())
                val filesVm: MemberFilesViewModel = viewModel(factory = MemberFilesViewModel.Factory)

                /* Pause GUI polling (health-check, recording/playback timers) when backgrounded,
                   refresh statuses on resume. */
                LifecycleStartEffect(Unit) {
                    vm.setForeground(true)
                    onStopOrDispose { vm.setForeground(false) }
                }

                var screen by rememberSaveable { mutableStateOf(Screen.ChannelList) }
                var filesReturnScreen by rememberSaveable { mutableStateOf(Screen.Chat) }
                val fileChannels by vm.channels.collectAsState()
                val radioCall by RadioCallController.state.collectAsState()
                val radioActivation by RadioCallController.activation.collectAsState()
                LaunchedEffect(radioActivation?.id) {
                    val activation = radioActivation ?: return@LaunchedEffect
                    val channels = withTimeoutOrNull(5000) { vm.channels.first { items -> items.any { it.id == activation.channelId } } }
                    if (RadioCallController.activation.value?.id != activation.id) return@LaunchedEffect
                    if (channels == null) { RadioCallController.activated(activation.id, false); return@LaunchedEffect }
                    screen = Screen.Chat
                    vm.enableRadioForCall(activation.channelId, activation.id) { success -> RadioCallController.activated(activation.id, success) }
                }
                BackHandler(radioCall != null && radioCall?.phase != RadioCallPhase.ENDED) {
                    if (radioCall?.phase != RadioCallPhase.PREPARING) RadioCallController.end()
                }
                LaunchedEffect(requestedFiles, fileChannels) {
                    requestedFiles?.let { target ->
                        val channel = fileChannels.find { it.id == target.group }
                        if (channel != null) {
                            requestedFiles = null; vm.selectChannel(channel); filesVm.openLocation(target)
                            filesReturnScreen = Screen.Chat; screen = Screen.Files
                        }
                    }
                }
                var showJoinDialog by rememberSaveable { mutableStateOf(false) }
                var showOpenLink by rememberSaveable { mutableStateOf(false) }
                val objectNavigation by vm.objectNavigation.collectAsState()
                val objectLinkError by vm.objectLinkError.collectAsState()
                LaunchedEffect(objectNavigation?.revision) {
                    objectNavigation?.let { pending ->
                        screen = Screen.Chat
                        if (pending.target.kind == 1) vm.showOriginal(pending.target.message, pending.revision)
                    }
                }
                if (showOpenLink) OpenLinkDialog(onOpen = { showOpenLink = false; vm.openLink(it) }, onDismiss = { showOpenLink = false })
                objectLinkError?.let { error ->
                    AlertDialog(onDismissRequest = vm::dismissObjectLinkError, text = { Text(stringResource(error.reason)) },
                        confirmButton = { if (error.retry) TextButton(onClick = { vm.openLink(error.url) }) { Text(stringResource(R.string.retry)) } },
                        dismissButton = { TextButton(onClick = vm::dismissObjectLinkError) { Text(stringResource(R.string.close)) } })
                }
                var inviteChannelId by rememberSaveable { mutableStateOf<String?>(null) }
                var videoMessage by remember { mutableStateOf<Message?>(null) }
                var cameraTargetGeneration by rememberSaveable { mutableStateOf(0L) }
                var cameraTargetId by rememberSaveable { mutableStateOf<String?>(null) }
                val cameraTarget = cameraTargetId?.let {
                    if (it.startsWith("dm:")) ConversationTarget.Pm(it.removePrefix("dm:"), cameraTargetGeneration) else ConversationTarget.Channel(it)
                }
                var photoMessage by remember { mutableStateOf<Message?>(null) }
                var captureMode by rememberSaveable { mutableStateOf(CaptureMode.VIDEO) }
                var hadCall by rememberSaveable { mutableStateOf(false) }
                var screenBeforeCall by rememberSaveable { mutableStateOf<Screen?>(null) }
                val snackbarHostState = remember { SnackbarHostState() }
                val scope = rememberCoroutineScope()
                val openOwnStream: (String) -> Unit = openOwnStream@{ group ->
                    val publisher = StreamController.state.value
                    if (publisher?.ongoing != true || publisher.group != group) {
                        LogManager.addLog("WARN", "stream", "own stream navigation stale group=$group publisher=${publisher?.id}")
                        return@openOwnStream
                    }
                    val channel = vm.channels.value.firstOrNull { it.id == group }
                    if (channel == null) {
                        LogManager.addLog("ERROR", "stream", "own stream channel unavailable group=$group publisher=${publisher.id}")
                        scope.launch { snackbarHostState.showSnackbar(getString(R.string.stream_group_closed)) }
                        return@openOwnStream
                    }
                    LogManager.addLog("INFO", "stream", "own stream navigation from=$screen channel=${vm.currentChannel.value?.id} " +
                        "target=${channel.id} publisher=${publisher.id}")
                    if (vm.currentChannel.value?.id != channel.id) vm.selectChannel(channel)
                    if (vm.currentChannel.value?.id != channel.id) {
                        LogManager.addLog("ERROR", "stream", "own stream channel selection failed group=$group")
                        return@openOwnStream
                    }
                    vm.selectStream(null); screen = Screen.Streams
                }
                val pmInvite by vm.pmInvite.collectAsState()
                val linkJoin by vm.linkJoin.collectAsState()
                val serverLink by vm.serverLink.collectAsState()
                val serverChannels by vm.channels.collectAsState()
                val joinedChannel by vm.joinedChannel.collectAsState()
                LaunchedEffect(joinedChannel) {
                    joinedChannel?.let { channel ->
                        vm.consumeJoinedChannel(); vm.selectChannel(channel); screen = Screen.Chat
                    }
                }

                val callState by vm.callState.collectAsState()

                // После смерти процесса ViewModel уже новый; сохранённый экран может потерять свою беседу или звонок.
                LaunchedEffect(screen) {
                    if (screen != Screen.Chat) vm.cancelObjectNavigation()
                    if (screen != Screen.MemberList && screen != Screen.Dm) vm.cancelDmStart()
                    inputScreen = screen.name
                    LogManager.addLog("INFO", "MAIN", "UI screen=$screen")
                    val unavailable = when (screen) {
                        Screen.Chat, Screen.MemberList, Screen.Files, Screen.Streams -> vm.currentChannel.value == null
                        Screen.Dm -> vm.currentDm.value == null
                        Screen.CameraRecord -> cameraTarget == null
                        Screen.Call -> callState == null
                        else -> false
                    }
                    if (unavailable) {
                        LogManager.addLog("WARN", "MAIN", "Restored screen=$screen has no conversation/call; returning to channel list")
                        screen = Screen.ChannelList
                    }
                }

                /* После звонка снимаем showWhenLocked входящего. Экран может гаснуть:
                   аудио и CPU удерживает CallService независимо от Activity. */
                LaunchedEffect(callState, radioCall) {
                    val st = callState
                    val inCall = st != null && st.phase != CallPhase.NONE &&
                        st.phase != CallPhase.ENDED
                    if (inCall || radioCall != null && radioCall?.phase != RadioCallPhase.ENDED) {
                        hadCall = true
                    } else {
                        if (hadCall) releaseLockScreen()
                    }
                }

                /* Звонок управляет навигацией: OUTGOING/RINGING/ACTIVE → экран Call (запоминаем
                   откуда пришли, чтобы свернуться обратно). ENDED не навигирует — CallScreen сам
                   показывает «Завершён», если пользователь уже на нём. INCOMING → диалог поверх. */
                var lastCallPhase by rememberSaveable { mutableStateOf<CallPhase?>(null) }
                LaunchedEffect(callState?.phase) {
                    val phase = callState?.phase
                    if (phase == lastCallPhase) return@LaunchedEffect
                    lastCallPhase = phase
                    when (phase) {
                        CallPhase.OUTGOING, CallPhase.RINGING, CallPhase.WAIT_DECISION,
                        CallPhase.ACTIVE -> {
                            if (phase == CallPhase.ACTIVE && (screen == Screen.Streams || screenBeforeCall == Screen.Streams))
                                screenBeforeCall = Screen.Chat
                            if (screen != Screen.Call && phase != CallPhase.ACTIVE) screenBeforeCall = screen
                            if (screen != Screen.Streams || phase == CallPhase.ACTIVE) screen = Screen.Call
                        }
                        null -> if (screen == Screen.Call) screen = screenBeforeCall ?: Screen.ChannelList
                        else -> {}
                    }
                }

                /* Автооткрытие DM-беседы после нажатия «Message» в списке участников. */
                LaunchedEffect(Unit) {
                    vm.dmOpen.collect { convId ->
                        if (convId != null) {
                            vm.consumeDmOpen()
                            if (screen == Screen.MemberList && vm.currentDm.value?.convId == convId) screen = Screen.Dm
                            else LogManager.addLog("DEBUG", "MAIN", "Ignoring stale DM navigation conv=$convId screen=$screen")
                        }
                    }
                }

                val showInvite: (Channel) -> Unit = { ch ->
                    if (!MemberFilesProtocol.validAddress(ch.id)) {
                        LogManager.addLog("ERROR", "INVITE", "invalid group address: ${ch.id}")
                        scope.launch { snackbarHostState.showSnackbar(AppLanguage.text(R.string.invalid_channel_id)) }
                    } else {
                        inviteChannelId = ch.id
                    }
                }

                BackHandler(enabled = screen != Screen.ChannelList && screen != Screen.Files) {
                    when (screen) {
                        Screen.MemberList -> screen = Screen.Chat
                        Screen.CameraRecord -> screen = if (cameraTarget is ConversationTarget.Pm) Screen.Dm else Screen.Chat
                        Screen.Streams -> { vm.selectStream(null); screen = Screen.Chat }
                        Screen.Chat -> screen = Screen.ChannelList
                        Screen.Dm -> { vm.closeDm(); screen = Screen.ChannelList }
                        Screen.Call -> screen = screenBeforeCall ?: Screen.ChannelList
                        else -> screen = Screen.ChannelList
                    }
                }

                BackHandler(enabled = videoMessage != null) {
                    videoMessage = null
                }

                BackHandler(enabled = photoMessage != null) {
                    photoMessage = null
                }

                val connected = NativeLib.isReady()
                val contentReady by vm.contentReady.collectAsState()
                LaunchedEffect(requestedLink, connected, contentReady) {
                    val link = requestedLink ?: return@LaunchedEffect
                    if (connected && contentReady) { requestedLink = null; vm.openLink(link) }
                }

                /* Subscribe native logs only when LogScreen is open */
                DisposableEffect(screen) {
                    if (screen == Screen.Logs) NativeLib.subscribeNativeLogs(true)
                    onDispose { NativeLib.subscribeNativeLogs(false) }
                }

                Column(modifier = Modifier.fillMaxSize()) {
                    val callPhase = callState?.phase
                    val showCallBanner = screen != Screen.Call &&
                        (callPhase == CallPhase.OUTGOING ||
                         callPhase == CallPhase.RINGING ||
                         callPhase == CallPhase.WAIT_DECISION ||
                         callPhase == CallPhase.ACTIVE)
                    if (showCallBanner) {
                        ActiveCallBanner(
                            viewModel = vm,
                            onOpen = { screen = Screen.Call },
                            onHangup = { vm.hangupCall() }
                        )
                    }
                    Box(
                        modifier = Modifier.weight(1f)
                            .then(if (showCallBanner) Modifier.consumeWindowInsets(WindowInsets.statusBars) else Modifier)
                    ) {
                        when (screen) {
                            Screen.ChannelList -> {
                                val channels by vm.channels.collectAsState()
                                val dmConversations by vm.dmConversations.collectAsState()
                                val dmDeleting by vm.dmDeleting.collectAsState()
                                val dmDeleteFailed by vm.dmDeleteFailed.collectAsState()
                                val stuck by vm.isStuck.collectAsState()
                                val cpuLoad by vm.cpuLoad.collectAsState()
                                val activeRadioGroup by vm.radioAudio.activeGroup.collectAsState()
                                ChannelListScreen(
                                    channels = channels,
                                    activeRadioGroup = activeRadioGroup,
                                    dmConversations = dmConversations,
                                    onDeleteDm = vm::deleteDmConversation,
                                    deletingDm = dmDeleting,
                                    dmDeleteFailed = dmDeleteFailed,
                                    onDismissDmDeleteError = vm::dismissDmDeleteError,
                                    connected = connected,
                                    isStuck = stuck,
                                    cpuLoad = cpuLoad,
                                    onCpuLoadVisible = vm::setCpuLoadVisible,
                                    onSelectChannel = { ch ->
                                        vm.selectChannel(ch)
                                        if (vm.currentChannel.value?.id == ch.id) screen = Screen.Chat
                                    },
                                    onSelectDm = { dm ->
                                        vm.openDm(dm)
                                        if (vm.currentDm.value?.convId == dm.convId) screen = Screen.Dm
                                    },
                                    onCreateChannel = { name, type -> vm.createChannel(name, "channel_1", type) },
                                    onJoinChannel = { showJoinDialog = true },
                                    onOpenLink = { showOpenLink = true },
                                    onInviteClick = showInvite,
                                    onDeleteChannel = { vm.deleteChannel(it.id) },
                                    onSettings = { screen = Screen.Settings },
                                    onLogs = { screen = Screen.Logs },
                                    onOwnStream = openOwnStream,
                                    onRefresh = { vm.requestRefresh() }
                                )
                            }
                            Screen.Chat -> {
                                val messages by vm.messages.collectAsState()
                                val channel = vm.currentChannel.collectAsState().value
                                if (channel != null) {
                                    if (channel.groupType == "kanban") KanbanScreen(channel,
                                        viewModel = vm, onViewPhoto = { photoMessage = it }, onPlayVideo = { videoMessage = it },
                                        onMembers = { screen = Screen.MemberList }, onInvite = { showInvite(channel) },
                                        onBack = { vm.cancelObjectNavigation(); screen = Screen.ChannelList })
                                    else if (channel.groupType != "chat") Text(stringResource(R.string.kanban_waiting_type))
                                    else ChatScreen(
                                        channel = channel,
                                        messages = messages,
                                        connected = connected,
                                        viewModel = vm,
                                        onMembersClick = { screen = Screen.MemberList },
                                        onFiles = { owner ->
                                            filesVm.open(channel.id, owner); filesReturnScreen = Screen.Chat; screen = Screen.Files
                                        },
                                        onInvite = { showInvite(channel) },
                                        onBack = { vm.cancelObjectNavigation(); screen = Screen.ChannelList },
                                        onStreams = { vm.selectStream(null); screen = Screen.Streams },
                                        onOwnStream = openOwnStream,
                                        onWatchStream = { vm.selectStream(it); screen = Screen.Streams },
                                        onWatchAllStreams = { vm.watchAllStreams(); screen = Screen.Streams },
                                        onPlayVideo = { msg -> videoMessage = msg },
                                        onViewPhoto = { msg -> photoMessage = msg },
                                        onRecordVideo = {
                                            if (com.utun.chat.data.StreamController.blocksCapture()) return@ChatScreen
                                            val target = ConversationTarget.Channel(channel.id)
                                            cameraTargetGeneration = 0; cameraTargetId = target.nativeId
                                            captureMode = CaptureMode.VIDEO
                                            vm.captureCameraReply(target); screen = Screen.CameraRecord
                                        },
                                        onCapturePhoto = {
                                            if (com.utun.chat.data.StreamController.blocksCapture()) return@ChatScreen
                                            val target = ConversationTarget.Channel(channel.id)
                                            cameraTargetGeneration = 0; cameraTargetId = target.nativeId; captureMode = CaptureMode.PHOTO
                                            vm.captureCameraReply(target); screen = Screen.CameraRecord
                                        }
                                    )
                                }
                            }
                            Screen.Streams -> {
                                vm.currentChannel.collectAsState().value?.let { channel ->
                                    val sources by vm.streamSources.collectAsState()
                                    val selected by vm.streamSelection.collectAsState()
                                    val viewAll by vm.streamViewAll.collectAsState()
                                    val failed by vm.streamSourcesFailed.collectAsState()
                                    StreamScreen(channel.id, channel.name, sources, selected, failed, vm::selectStream, viewAll) {
                                        vm.selectStream(null); screen = Screen.Chat
                                    }
                                }
                            }
                            Screen.MemberList -> {
                                val members by vm.members.collectAsState()
                                val selectedMember by vm.selectedMember.collectAsState()
                                val channel = vm.currentChannel.collectAsState().value
                                if (channel != null) {
                                    MemberListScreen(
                                        channel = channel,
                                        members = members,
                                        selectedMember = selectedMember,
                                        viewModel = vm,
                                        onFiles = { owner ->
                                            filesVm.open(channel.id, owner); filesReturnScreen = Screen.MemberList; screen = Screen.Files
                                        },
                                        onBack = { screen = Screen.Chat }
                                    )
                                }
                            }
                            Screen.Files -> {
                                val channel = vm.currentChannel.collectAsState().value
                                if (channel != null) MemberFilesScreen(channel, filesVm) { screen = filesReturnScreen }
                            }
                            Screen.Dm -> {
                                val dmMessages by vm.dmMessages.collectAsState()
                                val dm = vm.currentDm.collectAsState().value
                                if (dm != null) {
                                    DmChatScreen(
                                        conv = dm,
                                        messages = dmMessages,
                                        viewModel = vm,
                                        onRecordVideo = {
                                            if (com.utun.chat.data.StreamController.blocksCapture()) return@DmChatScreen
                                            val target = ConversationTarget.Pm(dm.convId, dm.generation)
                                            cameraTargetGeneration = target.generation; cameraTargetId = target.nativeId
                                            captureMode = CaptureMode.VIDEO
                                            vm.captureCameraReply(target); screen = Screen.CameraRecord
                                        },
                                        onCapturePhoto = {
                                            if (com.utun.chat.data.StreamController.blocksCapture()) return@DmChatScreen
                                            val target = ConversationTarget.Pm(dm.convId, dm.generation)
                                            cameraTargetGeneration = target.generation; cameraTargetId = target.nativeId
                                            captureMode = CaptureMode.PHOTO
                                            vm.captureCameraReply(target); screen = Screen.CameraRecord
                                        },
                                        onViewPhoto = { photoMessage = it },
                                        onPlayVideo = { videoMessage = it },
                                        onBack = { vm.closeDm(); screen = Screen.ChannelList }
                                    )
                                } else LaunchedEffect(Unit) {
                                    LogManager.addLog("INFO", "PM", "Current conversation closed; returning to channel list")
                                    screen = Screen.ChannelList
                                }
                            }
                            Screen.Settings -> SettingsScreen(vm = vm, onBack = { screen = Screen.ChannelList }, onExit = { finish() })
                            Screen.Logs -> LogScreen(onBack = { screen = Screen.ChannelList })
                            Screen.Call -> CallScreen(
                                viewModel = vm,
                                onClose = { vm.dismissCall(); screen = screenBeforeCall ?: Screen.ChannelList },
                                onMinimize = { screen = screenBeforeCall ?: Screen.ChannelList }
                            )
                            Screen.CameraRecord -> {
                                val target = cameraTarget
                                if (target != null) {
                                    CameraRecordScreen(initialMode = captureMode,
                                        onSend = { path, mode ->
                                            if (mode == CaptureMode.PHOTO) vm.sendPhotoMessage(path, target, vm.cameraReply)
                                            else vm.sendVideoMessage(path, target, vm.cameraReply)
                                            screen = if (target is ConversationTarget.Pm) Screen.Dm else Screen.Chat
                                        },
                                        onClose = { screen = if (target is ConversationTarget.Pm) Screen.Dm else Screen.Chat }
                                    )
                                }
                            }
                        }
                        SnackbarHost(snackbarHostState, Modifier.align(Alignment.BottomCenter).navigationBarsPadding())

                        videoMessage?.let { msg ->
                            if (msg.filePath.isNotEmpty()) {
                                VideoPlayerScreen(filePath = msg.filePath, onClose = { videoMessage = null }, onOpened = vm::attachmentOpened)
                            }
                        }

                        photoMessage?.let { msg ->
                            if (msg.filePath.isNotEmpty()) {
                                PhotoViewerScreen(filePath = msg.filePath, onClose = { photoMessage = null }, onOpened = vm::attachmentOpened)
                            }
                        }

                        if (callState?.phase == CallPhase.INCOMING) {
                            IncomingCallDialog(viewModel = vm)
                        }
                        radioCall?.let { RadioCallDialog(it) }
                        if (screen == Screen.Streams && callState?.phase in listOf(CallPhase.OUTGOING, CallPhase.RINGING, CallPhase.WAIT_DECISION)) {
                            CallScreen(viewModel = vm, onClose = { vm.dismissCall() }, onMinimize = {})
                        }
                    }
                }

                pmInvite?.let { state ->
                    PmInviteDialog(state, vm::selectPmInviteGroup, vm::sendPmInvite, vm::dismissPmInvite)
                }
                com.utun.chat.ui.screens.DevicePairingDialogs()
                val localInvitations by com.utun.chat.data.LocalDiscovery.invitations.collectAsState()
                localInvitations.firstOrNull()?.let { invitation ->
                    AlertDialog(
                        onDismissRequest = com.utun.chat.data.LocalDiscovery::dismissInvitation,
                        title = { Text(stringResource(R.string.invite_link)) },
                        text = { Text(stringResource(R.string.local_invite_received, invitation.device.name)) },
                        confirmButton = { TextButton(onClick = {
                            com.utun.chat.data.LocalDiscovery.dismissInvitation()
                            vm.openInviteLink(invitation.link)
                        }) { Text(stringResource(R.string.connect)) } },
                        dismissButton = { TextButton(onClick = com.utun.chat.data.LocalDiscovery::dismissInvitation) {
                            Text(stringResource(R.string.close))
                        } }
                    )
                }
                serverLink?.let { link ->
                    val channels = serverChannels.filter { NativeLib.isLocalChannelMember(it.id) }
                    AlertDialog(
                        onDismissRequest = vm::dismissServerLink,
                        title = { Text(stringResource(R.string.server_add_channel)) },
                        text = {
                            if (channels.isEmpty()) Text(stringResource(R.string.server_add_no_channels))
                            else LazyColumn(Modifier.heightIn(max = 320.dp)) {
                                items(channels.size) { index ->
                                    val channel = channels[index]
                                    TextButton(onClick = { vm.openInviteLink(link, channel.id) }) {
                                        Text(channel.name + " (" + channel.id + ")")
                                    }
                                }
                            }
                        },
                        confirmButton = {},
                        dismissButton = { TextButton(onClick = vm::dismissServerLink) { Text(stringResource(R.string.cancel)) } }
                    )
                }
                linkJoin?.let { state ->
                    AlertDialog(
                        onDismissRequest = vm::dismissLinkJoin,
                        title = { Text(stringResource(R.string.join_channel)) },
                        text = {
                            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                if (state.pending) {
                                    LinearProgressIndicator(Modifier.fillMaxWidth())
                                    Text(stringResource(R.string.connecting_peer))
                                } else {
                                    Text(when (state.result) { // CHAT_CONNECT_* из chat_sync.h.
                                        -11 -> stringResource(R.string.invite_not_allowed)
                                        -12 -> stringResource(R.string.server_add_bad_key)
                                        -10 -> stringResource(R.string.invite_authorization_failed)
                                        else -> if (state.peer.isEmpty()) stringResource(R.string.invalid_invite_link)
                                            else stringResource(R.string.invite_join_failed, state.result ?: -1)
                                    },
                                        color = MaterialTheme.colorScheme.error)
                                }
                            }
                        },
                        confirmButton = {
                            if (!state.pending && state.peer.isNotEmpty()) TextButton(onClick = {
                                vm.openInviteLink(state.link, if (state.serverAddition) state.channelId else "")
                            }) {
                                Text(stringResource(R.string.retry))
                            }
                        },
                        dismissButton = { TextButton(onClick = vm::dismissLinkJoin) { Text(stringResource(R.string.close)) } }
                    )
                }
                if (showJoinDialog) {
                    JoinChannelDialog(
                        onConnect = { data: InviteData ->
                            vm.openInviteLink(data.link)
                            showJoinDialog = false
                        },
                        onDismiss = { showJoinDialog = false }
                    )
                }

                inviteChannelId?.let { chId ->
                    DisposableEffect(chId) {
                        vm.requestInvite(chId)
                        onDispose { vm.clearInvite(chId) }
                    }
                    val candidates by vm.inviteCandidates.collectAsState()
                    val link by vm.inviteLink.collectAsState()
                    val pending by vm.inviteLinkPending.collectAsState()
                    InviteLinkDialog(
                        candidates = candidates,
                        link = link,
                        pending = pending,
                        onSelectNode = { nodeId -> vm.requestInviteLink(chId, nodeId) },
                        onDismiss = { vm.clearInvite(chId); inviteChannelId = null }
                    )
                }
            }
        }
    }

    override fun onStart() {
        MicrophonePermission.requester = microphoneRequester
        RadioOverlayService.overlayPermissionRequester = overlayPermissionRequester
        super.onStart()
        ChatApplication.instance.avatars.setForeground(true)
        RadioAudioEngine.instance().setForeground(true)
        if (intent.getBooleanExtra(MicrophonePermission.EXTRA_REQUEST, false)) {
            intent.removeExtra(MicrophonePermission.EXTRA_REQUEST)
            requestMicrophone("PTT") {
                RadioOverlayService.sync(this)
                Toast.makeText(this, AppLanguage.text(R.string.microphone_ptt_retry), Toast.LENGTH_LONG).show()
            }
        }
        CallController.setActivityVisible(true)
        RadioCallController.setVisible(true)
    }

    override fun onResume() {
        super.onResume()
        com.utun.chat.data.PhoneStateMonitor.refresh()
        RadioOverlayService.setActivityVisible(true)
    }

    override fun onPause() {
        releaseVolumePtt("activity-paused")
        RadioOverlayService.setActivityVisible(false)
        super.onPause()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        readLinkIntent(intent)
        RadioCallController.notificationIntent(intent)
        readFilesIntent(intent)
        if (intent.getBooleanExtra(MicrophonePermission.EXTRA_REQUEST, false)) {
            intent.removeExtra(MicrophonePermission.EXTRA_REQUEST)
            requestMicrophone("PTT") {
                RadioOverlayService.sync(this)
                Toast.makeText(this, AppLanguage.text(R.string.microphone_ptt_retry), Toast.LENGTH_LONG).show()
            }
        }
        if (incomingCallId(intent) != 0L) {
            LogManager.addLog("INFO", "MAIN", "launched for incoming call 0x%016x".format(incomingCallId(intent)))
            showOverLockScreen()
        }
    }

    private fun incomingCallId(intent: Intent?): Long =
        intent?.getLongExtra(IncomingCallManager.EXTRA_CALL_ID, 0L) ?: 0L

    private fun readFilesIntent(intent: Intent) {
        val group = intent.getStringExtra("files_group") ?: return
        val server = intent.getStringExtra("files_server").orEmpty()
        val owner = intent.getStringExtra("files_owner").orEmpty()
        val path = intent.getStringExtra("files_path") ?: ""
        intent.removeExtra("files_group")
        if (!MemberFilesProtocol.validAddress(group) || !MemberFilesProtocol.validAddress(server) || !MemberFilesProtocol.validPath(path) ||
            if (owner.isEmpty()) path.isNotEmpty() else !MemberFilesProtocol.validAddress(owner)) {
            LogManager.addLog("ERROR", "Files", "Invalid notification target group=$group server=$server owner=$owner path=$path"); return
        }
        requestedFiles = FileLocation(group, server, owner, path)
        LogManager.addLog("INFO", "Files", "Notification opened location=$requestedFiles")
    }

    /** Показать активити поверх блокировки и включить экран (входящий звонок). */
    private fun showOverLockScreen() {
        overLock = true
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            activityEffect("show when locked") { setShowWhenLocked(true) }
            activityEffect("turn screen on") { setTurnScreenOn(true) }
        } else {
            @Suppress("DEPRECATION")
            activityEffect("lock screen flags") { window.addFlags(WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED
                or WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON) }
        }
    }

    /** Снять «показ поверх блокировки», выставленный для входящего звонка. */
    private fun releaseLockScreen() {
        if (!overLock) return
        val released = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            val shown = activityEffect("clear show when locked") { setShowWhenLocked(false) }
            val wake = activityEffect("clear turn screen on") { setTurnScreenOn(false) }
            shown && wake
        } else {
            @Suppress("DEPRECATION")
            activityEffect("clear lock screen flags") { window.clearFlags(WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED
                or WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON) }
        }
        overLock = !released
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED) {
            activityEffect("notification permission request") { requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1) }
        }
    }

    override fun onStop() {
        if (MicrophonePermission.requester === microphoneRequester) MicrophonePermission.requester = null
        if (RadioOverlayService.overlayPermissionRequester === overlayPermissionRequester) RadioOverlayService.overlayPermissionRequester = null
        activityEffect("avatar background") { ChatApplication.instance.avatars.setForeground(false) }
        activityEffect("radio background") { RadioAudioEngine.instance().setForeground(false) }
        activityEffect("call background") { CallController.setActivityVisible(false) }
        activityEffect("radio call background") { RadioCallController.setVisible(false) }
        super.onStop()
    }

    override fun onDestroy() {
        microphoneAction = null
        super.onDestroy()
    }

    /** Независимый эффект Activity: отказ SDK логируется и не пропускает очистку остальных владельцев. */
    private fun activityEffect(name: String, action: () -> Unit): Boolean = try { action(); true }
        catch (e: Exception) { LogManager.addLog("ERROR", "MAIN", "Activity effect failed name=$name: $e"); false }

    /** Volume-кнопки (удержание) → PTT рации, при активной рации и включённой опции. */
    private val pttVolumeKeys = mutableSetOf<Int>()

    private fun releaseVolumePtt(reason: String) {
        if (pttVolumeKeys.isEmpty()) return
        pttVolumeKeys.clear()
        RadioAudioEngine.instance().cancelTalk(RadioPttSource.VOLUME)
        LogManager.addLog("INFO", "PTT", "Volume hold cancelled: $reason")
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        if (!hasFocus) releaseVolumePtt("window-focus-lost")
        super.onWindowFocusChanged(hasFocus)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val isVol = event.keyCode == KeyEvent.KEYCODE_VOLUME_UP || event.keyCode == KeyEvent.KEYCODE_VOLUME_DOWN
        val pttOn = volumePttEnabled && RadioAudioEngine.instance().isActive()
        if (!pttOn) releaseVolumePtt("disabled-or-radio-inactive")
        if (!pttOn || !isVol) return super.dispatchKeyEvent(event)
        when (event.action) {
            KeyEvent.ACTION_DOWN -> {
                if (event.repeatCount == 0 && event.keyCode !in pttVolumeKeys) {
                    if (pttVolumeKeys.isEmpty() && !RadioAudioEngine.instance().talkBegin(RadioPttSource.VOLUME)) return true
                    pttVolumeKeys.add(event.keyCode)
                    LogManager.addLog("DEBUG", "PTT", "key=${event.keyCode} down held=$pttVolumeKeys")
                }
                return true
            }
            KeyEvent.ACTION_UP -> {
                if (pttVolumeKeys.remove(event.keyCode) && pttVolumeKeys.isEmpty())
                    RadioAudioEngine.instance().talkEnd(RadioPttSource.VOLUME)
                LogManager.addLog("DEBUG", "PTT", "key=${event.keyCode} up held=$pttVolumeKeys cancelled=${event.isCanceled}")
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }
}
