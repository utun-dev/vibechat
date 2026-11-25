package com.utun.chat.ui.screens

import com.utun.chat.R
import kotlin.math.roundToInt
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.clickable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.Canvas
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import android.Manifest
import com.utun.chat.data.PhoneStateMonitor
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import com.utun.chat.ChatApplication
import com.utun.chat.data.NativeLib
import com.utun.chat.ui.components.ProfileAvatarControls
import com.utun.chat.ui.components.PortableRadioIcon
import com.utun.chat.data.NotificationSound
import com.utun.chat.data.IncomingCallManager
import com.utun.chat.data.RadioOverlayService
import com.utun.chat.data.RadioAudioEngine
import com.utun.chat.data.RadioAutoSettings
import com.utun.chat.data.ConfigProvider
import com.utun.chat.data.StreamFecParameters
import com.utun.chat.data.UdpLogSettings
import com.utun.chat.data.MonitoringSettings
import com.utun.chat.data.CallVideoBitrateRange
import com.utun.chat.data.MicrophonePermission
import com.utun.chat.data.LogManager
import com.utun.chat.data.ServerEntry
import com.utun.chat.data.AutoSocketProtocols
import com.utun.chat.data.LocalSocketState
import com.utun.chat.ui.components.LocalSocketNatInfo
import com.utun.chat.data.VoiceSettings
import com.utun.chat.data.NoiseSuppressionSettings
import com.utun.chat.headless.HeadlessService
import com.utun.chat.ui.components.BatteryOptimizationSetting
import com.utun.chat.ui.components.LanguageSetting
import com.utun.chat.viewmodel.ChatViewModel
import android.content.Intent
import androidx.activity.compose.BackHandler
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LifecycleResumeEffect
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.first
import java.util.UUID

private val LEVELS = listOf("DISABLED", "NONE", "ERROR", "WARN", "INFO", "DEBUG", "TRACE")
private val PROTOCOLS = listOf("tcp", "udp")

private fun parseDebugPreferences(text: String): Map<String, String> = text.split(',').mapNotNull { entry ->
    val pair = entry.trim().split('=', limit = 2)
    if (pair.size == 2 && pair[0].isNotBlank() && pair[1].uppercase() in LEVELS) pair[0].trim() to pair[1].uppercase()
    else { if (entry.isNotBlank()) LogManager.addLog("WARN", "CFG", "Malformed debug category preference ignored"); null }
}.toMap()

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(vm: ChatViewModel, onBack: () -> Unit, onExit: () -> Unit = {}) {
    val provider = ChatApplication.instance.configProvider
    val scope = rememberCoroutineScope()
    val changes = provider.changes
    val changeFailed by changes.failed.collectAsState()
    var loaded by remember { mutableStateOf(false) }
    var loadFailed by remember { mutableStateOf(false) }
    var loadAttempt by remember { mutableIntStateOf(0) }
    LaunchedEffect(loadAttempt) {
        loadFailed = false
        try { changes.awaitIdle(); withContext(Dispatchers.IO) { provider.allValues.first() }; loaded = true }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { loadFailed = true; LogManager.addLog("ERROR", "Settings", "Settings load failed: $e") }
    }
    BackHandler(enabled = !loaded, onBack = onBack)
    if (!loaded) {
        Column(Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            TextButton(onClick = onBack) { Text(stringResource(R.string.back)) }
            if (loadFailed) {
                Text(stringResource(R.string.settings_update_failed), color = MaterialTheme.colorScheme.error)
                Button(onClick = { loadAttempt++ }) { Text(stringResource(R.string.retry)) }
            } else CircularProgressIndicator()
        }
        return
    }
    var dirty by rememberSaveable { mutableStateOf(false) }
    var applyingConfig by remember { mutableStateOf(false) }
    var applyFailed by remember { mutableStateOf(false) }

    var tabIndex by rememberSaveable { mutableStateOf(0) }
    val tabs = listOf(R.string.settings to Icons.Default.Settings, R.string.audio to Icons.Default.VolumeUp,
        R.string.storage to Icons.Default.Storage, R.string.radio to null,
        R.string.broadcast to Icons.Default.LiveTv, R.string.dev to Icons.Default.Code, R.string.bluetooth to Icons.Default.Bluetooth)

    val handleBack: () -> Unit = back@{
        if (applyingConfig) return@back
        if (dirty) {
            applyingConfig = true
            scope.launch {
                try {
                    changes.awaitIdle()
                    check(!changes.failed.value) { "A settings change failed before restart" }
                    HeadlessService.awaitStartup()
                    if (NativeLib.isReady()) {
                        val config = provider.buildConfigText()
                        withContext(Dispatchers.IO) {
                            val path = provider.getString("db.path") ?: "utun_chat.db"
                            val port = provider.getInt("control.port")
                            check(NativeLib.restart(path, port, config.await())) { "core restart rejected settings" }
                        }
                    }
                    onBack()
                } catch (e: CancellationException) { throw e
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "CFG", "apply settings failed: ${e.javaClass.simpleName}: ${e.message}")
                    applyFailed = true
                } finally { applyingConfig = false }
            }
        } else onBack()
    }

    BackHandler(onBack = handleBack)
    if (changeFailed) AlertDialog(onDismissRequest = changes::dismissFailure,
        text = { Text(stringResource(R.string.settings_update_failed)) }, confirmButton = {
            TextButton(onClick = changes::dismissFailure) { Text(stringResource(R.string.close)) }
        })
    if (applyingConfig) AlertDialog(onDismissRequest = {}, title = { Text(stringResource(R.string.settings)) },
        text = { CircularProgressIndicator() }, confirmButton = {})
    if (applyFailed) AlertDialog(onDismissRequest = { applyFailed = false }, title = { Text(stringResource(R.string.settings)) },
        text = { Text(stringResource(R.string.settings_update_failed)) }, confirmButton = {
            TextButton(onClick = { applyFailed = false }) { Text(stringResource(R.string.close)) }
        })

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(tabs[tabIndex].first)) },
                navigationIcon = { IconButton(onClick = handleBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back)) } }
            )
        }
    ) { padding ->
        Column(modifier = Modifier.padding(padding)) {
            TabRow(selectedTabIndex = tabIndex) {
                tabs.forEachIndexed { idx, (name, image) ->
                    val title = stringResource(name)
                    TooltipBox(positionProvider = TooltipDefaults.rememberPlainTooltipPositionProvider(),
                        tooltip = { PlainTooltip { Text(title) } }, state = rememberTooltipState()) {
                        Tab(selected = tabIndex == idx, onClick = {
                            LogManager.addLog("DEBUG", "Settings", "Section selected index=$idx")
                            tabIndex = idx
                        }, icon = {
                            if (image == null) PortableRadioIcon(title)
                            else Icon(image, contentDescription = title, modifier = Modifier.size(24.dp))
                        })
                    }
                }
            }
            key(tabIndex) {
                var tabReady by remember { mutableStateOf(false) }
                LaunchedEffect(Unit) { changes.awaitIdle(); tabReady = true }
                if (tabReady) when (tabIndex) {
                0 -> GeneralSettingsTab(onDirty = { dirty = it })
                1 -> AudioSettingsTab(vm)
                2 -> StorageSettingsTab()
                3 -> RadioSettingsTab()
                4 -> StreamSettingsTab()
                5 -> DeveloperSettingsTab(vm, onDirty = { dirty = it }, onExit)
                6 -> BluetoothSettingsTab()
                } else CircularProgressIndicator(Modifier.padding(16.dp))
            }
        }
    }
}

/* Параметры RS сохраняются для следующей эпохи трансляции. */
@Composable
private fun StreamSettingsTab() {
    val provider = ChatApplication.instance.configProvider
    var subblocks by rememberSaveable { mutableStateOf(provider.getInt("chat.stream_subblocks").toString()) }
    var redundant by rememberSaveable { mutableStateOf(provider.getInt("chat.stream_rs_redundant").toString()) }
    var stallTimeout by rememberSaveable { mutableStateOf(provider.getInt("chat.stream_stall_timeout_ms").toString()) }
    var maxLiveLag by rememberSaveable { mutableStateOf(provider.getInt("chat.stream_max_live_lag_ms").toString()) }
    var budgetTexts by rememberSaveable {
        mutableStateOf(StreamFecParameters.budgets.map { provider.getInt("chat.${it.key}").toString() })
    }
    val budgetLabels = listOf(R.string.stream_regular_limit, R.string.stream_temporary_limit, R.string.stream_background_limit,
        R.string.stream_peer_temporary_limit, R.string.stream_peer_limit, R.string.stream_control_limit, R.string.stream_extra_receive_limit)
    var saving by remember { mutableStateOf(false) }
    var failed by remember { mutableStateOf(false) }
    var saved by remember { mutableStateOf(false) }
    val n = subblocks.toIntOrNull()
    val r = redundant.toIntOrNull()
    val timeout = stallTimeout.toIntOrNull()
    val liveLag = maxLiveLag.toIntOrNull()
    val budgets = StreamFecParameters.budgets.mapIndexed { i, budget -> budget.key to (budgetTexts[i].toIntOrNull() ?: -1) }.toMap()
    val fecValid = StreamFecParameters.valid(n, r, budgets.getValue("stream_regular_limit"))
    val valid = fecValid && timeout != null && timeout in 100..60000
        && liveLag != null && liveLag in 1000..60000 && StreamFecParameters.validBudgets(budgets)
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(stringResource(R.string.broadcast), style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(
            value = subblocks, onValueChange = { subblocks = it; saved = false }, enabled = !saving,
            label = { Text(stringResource(R.string.stream_subblocks)) }, singleLine = true,
            isError = !fecValid,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
        )
        OutlinedTextField(
            value = redundant, onValueChange = { redundant = it; saved = false }, enabled = !saving,
            label = { Text(stringResource(R.string.stream_rs_redundant)) }, singleLine = true,
            isError = !fecValid,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
        )
        Text(stringResource(R.string.stream_settings_hint))
        OutlinedTextField(
            value = stallTimeout, onValueChange = { stallTimeout = it; saved = false }, enabled = !saving,
            label = { Text(stringResource(R.string.stream_stall_timeout)) }, singleLine = true,
            isError = timeout == null || timeout !in 100..60000,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
        )
        Text(stringResource(R.string.stream_stall_timeout_hint))
        OutlinedTextField(
            value = maxLiveLag, onValueChange = { maxLiveLag = it; saved = false }, enabled = !saving,
            label = { Text(stringResource(R.string.stream_max_live_lag)) }, singleLine = true,
            isError = liveLag == null || liveLag !in 1000..60000,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
        )
        Text(stringResource(R.string.stream_max_live_lag_hint))
        StreamFecParameters.budgets.forEachIndexed { index, budget ->
            OutlinedTextField(
                value = budgetTexts[index], onValueChange = { text ->
                    budgetTexts = budgetTexts.mapIndexed { i, old -> if (i == index) text else old }; saved = false
                }, enabled = !saving, label = { Text(stringResource(budgetLabels[index])) }, singleLine = true,
                isError = budgets.getValue(budget.key) !in budget.range,
                supportingText = { Text("${budget.range.first}–${budget.range.last}") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number)
            )
        }
        Text(stringResource(R.string.stream_budgets_hint))
        if (!valid) Text(stringResource(R.string.stream_settings_invalid), color = MaterialTheme.colorScheme.error)
        Button(enabled = valid && !saving, onClick = {
            val total = n ?: return@Button
            val parity = r ?: return@Button
            val pause = timeout ?: return@Button
            val lag = liveLag ?: return@Button
            saving = true; failed = false; saved = false
            provider.changes.submit("broadcast parameters") {
                try {
                    provider.saveStreamSettings(total, parity, pause, lag, budgets)
                    NativeLib.setChatSetting("stream_subblocks", total.toString())
                    NativeLib.setChatSetting("stream_rs_redundant", parity.toString())
                    NativeLib.setChatSetting("stream_stall_timeout_ms", pause.toString())
                    NativeLib.setChatSetting("stream_max_live_lag_ms", lag.toString())
                    budgets.forEach { (key, value) -> NativeLib.setChatSetting(key, value.toString()) }
                    saved = true
                } catch (e: CancellationException) { throw e }
                catch (e: Exception) {
                    failed = true
                    LogManager.addLog("ERROR", "STREAM", "Broadcast settings save failed: $e")
                    throw e
                } finally {
                    saving = false
                }
            }
        }) { Text(stringResource(R.string.save)) }
        if (failed) Text(stringResource(R.string.stream_settings_save_failed), color = MaterialTheme.colorScheme.error)
        if (saved) Text(stringResource(R.string.stream_settings_saved))
    }
}

/* Основные настройки профиля и приложения. */

@Composable
private fun GeneralSettingsTab(onDirty: (Boolean) -> Unit) {
    val provider = ChatApplication.instance.configProvider
    var nodeName by remember { mutableStateOf("") }
    var localInvitesEnabled by remember { mutableStateOf(provider.getInt("chat.local_invites_enabled") != 0) }
    var localBroadcastsEnabled by remember { mutableStateOf(provider.getInt("chat.local_broadcasts_enabled") != 0) }

    LaunchedEffect(Unit) {
        try {
            nodeName = provider.getString("node.name") ?: provider.getDeviceName()
            LogManager.addLog("INFO", "SETTINGS", "General settings opened")
        } catch (e: CancellationException) { throw e }
        catch (e: Exception) { provider.changes.reportFailure("load general settings", e) }
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        item(key = "language") { LanguageSetting() }
        item(key = "battery") { BatteryOptimizationSetting(source = "settings") }
        item(key = "node-name") {
            OutlinedTextField(nodeName, { v -> onDirty(true); nodeName = v; provider.changes.submit("node name") { provider.setValue("node.name", v) } },
                label = { Text(stringResource(R.string.node_name)) }, singleLine = true, modifier = Modifier.heightIn(min = 40.dp))
        }
        item(key = "profile-avatar") { ProfileAvatarControls() }
        item(key = "local-network-title") {
            HorizontalDivider(Modifier.padding(vertical = 8.dp))
            Text(stringResource(R.string.local_network), style = MaterialTheme.typography.titleMedium)
        }
        item(key = "local-invites") {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.local_invites_enabled), Modifier.weight(1f))
                Switch(checked = localInvitesEnabled, onCheckedChange = { enabled ->
                    provider.changes.submit("local invitations") {
                        val value = if (enabled) "1" else "0"
                        provider.setValue("chat.local_invites_enabled", value)
                        if (NativeLib.isReady()) NativeLib.setChatSetting("local_invites_enabled", value)
                        localInvitesEnabled = enabled
                    }
                })
            }
        }
        item(key = "local-broadcasts") {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.local_broadcasts_enabled), Modifier.weight(1f))
                Switch(checked = localBroadcastsEnabled, onCheckedChange = { enabled ->
                    provider.changes.submit("local broadcasts") {
                        val value = if (enabled) "1" else "0"
                        provider.setValue("chat.local_broadcasts_enabled", value)
                        if (NativeLib.isReady()) NativeLib.setChatSetting("local_broadcasts_enabled", value)
                        localBroadcastsEnabled = enabled
                    }
                })
            }
        }
    }
}

/* Настройки разработчика: сеть, ключи, standby, диагностика и управление сервисом. */

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DeveloperSettingsTab(vm: ChatViewModel, onDirty: (Boolean) -> Unit, onExit: () -> Unit) {
    val provider = ChatApplication.instance.configProvider
    val scope = rememberCoroutineScope()

    var controlPort by remember { mutableStateOf("9999") }
    var monitoringEnabled by remember { mutableStateOf(false) }
    var monitoringLoaded by remember { mutableStateOf(false) }
    var controlBindIp by remember { mutableStateOf("127.0.0.1") }
    var ctrlBindLabel by remember { mutableStateOf("lo (127.0.0.1)") }
    var debugLevel by remember { mutableStateOf("info") }
    var logUdpIp by remember { mutableStateOf("") }
    var logUdpPort by remember { mutableStateOf("") }
    var logUdpEnabled by remember { mutableStateOf(false) }
    var logUdpLoaded by remember { mutableStateOf(false) }
    var standbyActiveSec by remember { mutableStateOf("2") }
    var standbySleepSec by remember { mutableStateOf("60") }
    var standbyMinSleepSec by remember { mutableStateOf("15") }
    var standbyKeepaliveMs by remember { mutableStateOf("30000") }
    val pubKey by vm.pubKey.collectAsState()
    val generatingKeys by vm.generatingKeys.collectAsState()

    var showDebugCategories by remember { mutableStateOf(false) }
    var showOverwriteKeys by remember { mutableStateOf(false) }
    var showClearDb by remember { mutableStateOf(false) }
    val databaseReset by vm.databaseReset.collectAsState()
    val clearingDb = databaseReset.running
    var catLevels by remember { mutableStateOf<Map<String, String>>(emptyMap()) }
    val debugCategories = remember { NativeLib.getDebugCategories() }
    var servers by remember { mutableStateOf<List<ServerEntry>>(emptyList()) }
    var serversLoaded by remember { mutableStateOf(false) }
    var serverSaveRevision by remember { mutableStateOf(0L) }
    var configFailure by remember { mutableStateOf(false) }
    var availableIfaces by remember { mutableStateOf<List<Pair<String, String>>>(emptyList()) }
    var connectionMode by remember { mutableStateOf("manual") }
    var autoSocketProtocols by remember { mutableStateOf(AutoSocketProtocols.BOTH) }
    var connectionOrder by remember { mutableStateOf(provider.getConnectionOrder()) }
    var connectionOrderMenu by remember { mutableStateOf(false) }
    var connectionSeconds by remember { mutableStateOf(connectionOrder.substringAfter("-", "2")) }
    var savingConnectionOrder by remember { mutableStateOf(false) }
    fun saveConnectionOrder(value: String) {
        savingConnectionOrder = true
        provider.changes.submit("connection order") {
            try {
                provider.setConnectionOrder(value)
                connectionOrder = value
                connectionSeconds = value.substringAfter("-", "4")
            }
            finally { savingConnectionOrder = false }
        }
    }
    var savingSocketProtocols by remember { mutableStateOf(false) }
    var hideMySockets by remember { mutableStateOf(provider.getHideMySockets()) }
    var savingHideMySockets by remember { mutableStateOf(false) }
    var autoConnect by remember { mutableStateOf(true) }

    val localSocketStates by vm.localSockets.collectAsState()
    val serviceEnabled by remember { provider.changes.observe("service preference", provider.serviceEnabledFlow) }.collectAsState(initial = true)
    val startOnBoot by remember { provider.changes.observe("boot preference", provider.startOnBootFlow) }.collectAsState(initial = true)
    var savingStartOnBoot by remember { mutableStateOf(false) }
    var startOnBootError by remember { mutableStateOf(false) }

    fun saveUdpLogSettings() {
        val settings = UdpLogSettings(logUdpEnabled, logUdpIp, logUdpPort.toIntOrNull() ?: 0)
        val request = provider.saveUdpLogSettings(settings)
        provider.changes.submit("network logging") {
            request.await()
            NativeLib.setUdpLogTarget(if (settings.active) settings.ip else "", if (settings.active) settings.port else 0)
            LogManager.addLog(if (settings.enabled && !settings.active) "WARN" else "INFO", "UDPLog",
                "Network logging enabled=${settings.enabled} active=${settings.active} target=${settings.ip}:${settings.port}")
        }
    }

    fun saveMonitoring() {
        val settings = MonitoringSettings(monitoringEnabled, controlBindIp, controlPort.toIntOrNull() ?: 0)
        if (settings.enabled && !settings.valid) return // Незавершённый ввод подсвечивается; последний верный endpoint сохранён.
        onDirty(true)
        val request = provider.saveMonitoringSettings(settings)
        provider.changes.submit("monitoring") { request.await() }
    }

    LaunchedEffect(Unit) {
        try {
            val monitoring = provider.getMonitoringSettings()
            monitoringEnabled = monitoring.enabled; controlPort = monitoring.port.toString(); controlBindIp = monitoring.ip
            monitoringLoaded = true
            val allIface = listOf("lo" to "127.0.0.1") + withContext(Dispatchers.IO) { provider.getAvailableInterfaces() }
            ctrlBindLabel = allIface.firstOrNull { it.second == controlBindIp }?.let { "${it.first} (${it.second})" } ?: controlBindIp
            debugLevel = provider.getString("debug.console_level") ?: "info"
            val udpLog = try { provider.getUdpLogSettings() }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) { provider.changes.reportFailure("load network logging", e); null }
            if (udpLog != null) {
                logUdpEnabled = udpLog.enabled; logUdpIp = udpLog.ip; logUdpPort = udpLog.port.toString()
                logUdpLoaded = true
                LogManager.addLog("INFO", "UDPLog", "Settings loaded enabled=${udpLog.enabled} active=${udpLog.active}")
            }
            standbyActiveSec = provider.getInt("chat.standby_active_sec").toString()
            standbySleepSec = provider.getInt("chat.standby_sleep_sec").toString()
            standbyMinSleepSec = provider.getInt("chat.standby_min_sleep_sec").toString()
            standbyKeepaliveMs = provider.getInt("chat.standby_keepalive_ms").toString()
            val stored = provider.getString("debug.categories") ?: ""
            catLevels = parseDebugPreferences(stored)
            try { servers = provider.loadServersFromConfig().await()
                serversLoaded = true
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CFG", "load network settings failed: ${e.javaClass.simpleName}: ${e.message}")
                configFailure = true
            }
            availableIfaces = allIface.drop(1)
            connectionMode = provider.getConnectionMode()
            autoSocketProtocols = provider.getAutoSocketProtocols()
            autoConnect = provider.getInt("chat.group_autoconnect") != 0
        } catch (e: CancellationException) { throw e }
        catch (e: Exception) { configFailure = true; provider.changes.reportFailure("load developer settings", e) }
    }

    val save: (List<ServerEntry>) -> Unit = { s ->
        onDirty(true)
        servers = s
        val revision = ++serverSaveRevision
        val request = provider.saveServersToConfig(s)
        provider.changes.submit("manual sockets") {
            try { request.await()
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CFG", "save network settings failed: ${e.javaClass.simpleName}: ${e.message}")
                if (revision == serverSaveRevision) configFailure = true
                throw e
            }
        }
    }
    val switchMode: (String) -> Unit = { mode ->
        onDirty(true)
        provider.changes.submit("connection mode") {
            try {
                provider.setConnectionMode(mode)
                connectionMode = mode
                if (NativeLib.isReady()) check(NativeLib.restart(provider.getString("db.path") ?: "",
                    provider.getInt("control.port"), provider.buildConfigText().await())) { "core restart rejected connection mode" }
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CFG", "connection mode change failed: ${e.javaClass.simpleName}: ${e.message}")
                withContext(Dispatchers.Main) { configFailure = true }
                throw e
            }
        }
    }
    val setDebug: (String) -> Unit = { level ->
        provider.changes.submit("debug level") {
            provider.setValue("debug.console_level", level)
            NativeLib.setDebugLevel("", level); debugLevel = level
        }
    }
    val setCatLevel: (String, String) -> Unit = { cat, lvl ->
        provider.changes.submit("debug category") {
            val updated = parseDebugPreferences(provider.getString("debug.categories").orEmpty()).toMutableMap()
            if (lvl.isEmpty()) updated.remove(cat) else updated[cat] = lvl
            val csv = updated.entries.joinToString(",") { "${it.key}=${it.value}" }
            provider.setValue("debug.categories", csv); catLevels = updated
            NativeLib.setDebugLevel(cat, lvl.ifEmpty { "NONE" })
        }
    }

    var ctrlExpanded by remember { mutableStateOf(false) }
    val ctrlIfaces = (listOf("lo" to "127.0.0.1") + availableIfaces.filter { !it.second.contains(':') }).distinct()
    val listState = rememberLazyListState()
    val openedAt = remember { android.os.SystemClock.uptimeMillis() }
    LaunchedEffect(Unit) {
        withFrameNanos { }
        LogManager.addLog("INFO", "SETTINGS", "Dev settings displayed: elapsedMs=${android.os.SystemClock.uptimeMillis() - openedAt} " +
            "visibleItems=${listState.layoutInfo.visibleItemsInfo.size} totalItems=${listState.layoutInfo.totalItemsCount}")
    }

    // Создаём и раскладываем только видимые настройки, включая динамические интерфейсы и категории.
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        state = listState,
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        item(key = "developer-title") {
            Text(stringResource(R.string.developer_settings), style = MaterialTheme.typography.titleMedium)
            Text(stringResource(R.string.developer_settings_warning), style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        item(key = "monitoring-enabled") {
            Row(Modifier.fillMaxWidth().toggleable(value = monitoringEnabled, enabled = monitoringLoaded,
                role = Role.Checkbox, onValueChange = { enabled ->
                    monitoringEnabled = enabled
                    if (!enabled) {
                        controlBindIp = "127.0.0.1"; controlPort = "9999"; ctrlBindLabel = "lo (127.0.0.1)"; ctrlExpanded = false
                    }
                    saveMonitoring()
                }), verticalAlignment = Alignment.CenterVertically) {
                Checkbox(checked = monitoringEnabled, onCheckedChange = null, enabled = monitoringLoaded)
                Column {
                    Text(stringResource(R.string.monitoring_enabled))
                    Text(stringResource(R.string.monitoring_description), style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        if (monitoringEnabled) item(key = "control") {
            Row(verticalAlignment = Alignment.CenterVertically) {
                ExposedDropdownMenuBox(expanded = ctrlExpanded, onExpandedChange = { ctrlExpanded = it }, modifier = Modifier.weight(1f)) {
                    OutlinedTextField(ctrlBindLabel, {}, readOnly = true, singleLine = true,
                        label = { Text(stringResource(R.string.control_bind_ip)) },
                        trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(ctrlExpanded) },
                        modifier = Modifier.menuAnchor().heightIn(min = 40.dp))
                    ExposedDropdownMenu(ctrlExpanded, onDismissRequest = { ctrlExpanded = false }) {
                        ctrlIfaces.forEach { (n, ip) ->
                            DropdownMenuItem(text = { Text("$n ($ip)") }, onClick = {
                                controlBindIp = ip; ctrlBindLabel = "$n ($ip)"; saveMonitoring(); ctrlExpanded = false
                            })
                        }
                    }
                }
                Spacer(Modifier.width(8.dp))
                OutlinedTextField(controlPort, { v -> controlPort = v; saveMonitoring() },
                    label = { Text(stringResource(R.string.port)) }, modifier = Modifier.width(80.dp).heightIn(min = 40.dp),
                    isError = (controlPort.toIntOrNull() ?: 0) !in 1..65535,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), singleLine = true)
            }
        }

        /* Keys */
        item(key = "keys-title") { Text(stringResource(R.string.keys), style = MaterialTheme.typography.titleMedium) }
        item(key = "public-key") { OutlinedTextField(pubKey, {}, label = { Text(stringResource(R.string.public_key)) }, readOnly = true, singleLine = true) }
        item(key = "key-actions") {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { showOverwriteKeys = true },
                    enabled = !generatingKeys) { Text(stringResource(if (generatingKeys) R.string.generating else R.string.generate_keys)) }
                Button(onClick = { showClearDb = true },
                    colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)) {
                    Icon(Icons.Default.Delete, null, modifier = Modifier.size(18.dp)); Spacer(Modifier.width(4.dp)); Text(stringResource(R.string.clear_db))
                }
            }
        }

        item(key = "network-divider") { HorizontalDivider(Modifier.padding(vertical = 8.dp)) }

        /* ── Connection Mode ── */
        item(key = "network-title") { Text(stringResource(R.string.network_interfaces), style = MaterialTheme.typography.titleMedium) }
        item(key = "connection-mode") {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(selected = connectionMode == "auto", enabled = serversLoaded,
                    onClick = { if (connectionMode != "auto") switchMode("auto") },
                    label = { Text(stringResource(R.string.automatic)) })
                FilterChip(selected = connectionMode == "manual", enabled = serversLoaded,
                    onClick = { if (connectionMode != "manual") switchMode("manual") },
                    label = { Text(stringResource(R.string.manual)) })
            }
        }

        item(key = "hide-my-sockets") {
            Column {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(stringResource(R.string.hide_my_sockets), modifier = Modifier.weight(1f))
                    Switch(checked = hideMySockets, enabled = !savingHideMySockets, onCheckedChange = { value ->
                        if (value != hideMySockets) {
                            savingHideMySockets = true
                            provider.changes.submit("hide my sockets") {
                                try {
                                    withContext(Dispatchers.IO) {
                                        provider.setHideMySockets(value)
                                        if (NativeLib.isReady()) {
                                            LogManager.addLog("INFO", "CFG", "Restarting core after hide_my_sockets change")
                                            check(NativeLib.restart(provider.getString("db.path") ?: "",
                                                provider.getInt("control.port"), provider.buildConfigText().await())) {
                                                "restart after hide_my_sockets change failed"
                                            }
                                            LogManager.addLog("INFO", "CFG", "Socket privacy restart completed")
                                        }
                                    }
                                    hideMySockets = value
                                } catch (e: CancellationException) { throw e
                                } catch (e: Exception) {
                                    LogManager.addLog("ERROR", "CFG", "socket privacy change failed: ${e.message}")
                                    configFailure = true
                                    throw e
                                } finally { savingHideMySockets = false }
                            }
                        }
                    })
                }
                Text(stringResource(R.string.hide_my_sockets_help), style = MaterialTheme.typography.bodySmall)
            }
        }
        item(key = "connection-order") {
            Column {
                Text(stringResource(R.string.connection_order_title))
                Text(stringResource(R.string.connection_order_help), style = MaterialTheme.typography.bodySmall)
                val letters = connectionOrder.takeWhile { it in "TUR" }
                val mode = connectionOrder.drop(letters.length).first()
                Box {
                    OutlinedButton(enabled = !savingConnectionOrder, onClick = { connectionOrderMenu = true }) { Text(letters) }
                    DropdownMenu(expanded = connectionOrderMenu, onDismissRequest = { connectionOrderMenu = false }) {
                        listOf("TUR", "TRU", "UTR", "URT", "RTU", "RUT", "TU", "TR", "UT", "UR", "RT", "RU", "T", "U", "R").forEach { order ->
                            DropdownMenuItem(text = { Text(order) }, onClick = {
                                connectionOrderMenu = false
                                saveConnectionOrder(order + connectionOrder.drop(letters.length))
                            })
                        }
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(R.string.connection_first, R.string.connection_sequential, R.string.connection_parallel).forEachIndexed { index, label ->
                        val selectedMode = ('1'.code + index).toChar()
                        FilterChip(selected = mode == selectedMode, enabled = !savingConnectionOrder,
                            onClick = { saveConnectionOrder(letters + selectedMode + if (selectedMode == '2') "-2" else "") },
                            label = { Text(stringResource(label)) })
                    }
                }
                if (mode == '2') {
                    OutlinedTextField(value = connectionSeconds, onValueChange = { connectionSeconds = it },
                        label = { Text(stringResource(R.string.connection_seconds)) },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
                    TextButton(enabled = !savingConnectionOrder && connectionSeconds.toIntOrNull() in 1..3600,
                        onClick = { saveConnectionOrder(letters + "2-" + connectionSeconds.toInt()) }) {
                        Text(stringResource(R.string.save))
                    }
                }
            }
        }
        if (connectionMode == "auto") {
            item(key = "auto-socket-protocols") {
                Column {
                    Text(stringResource(R.string.socket_protocols), style = MaterialTheme.typography.bodyMedium)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        AutoSocketProtocols.values().forEach { protocols ->
                            FilterChip(selected = autoSocketProtocols == protocols, enabled = serversLoaded && !savingSocketProtocols,
                                onClick = {
                                    if (autoSocketProtocols != protocols) {
                                        savingSocketProtocols = true
                                        provider.changes.submit("socket protocols") {
                                            try {
                                                withContext(Dispatchers.IO) {
                                                    provider.setAutoSocketProtocols(protocols)
                                                    if (NativeLib.isReady()) check(NativeLib.restart(provider.getString("db.path") ?: "",
                                                        provider.getInt("control.port"), provider.buildConfigText().await())) {
                                                        "restart after socket protocol change failed"
                                                    }
                                                }
                                                autoSocketProtocols = protocols
                                            } catch (e: CancellationException) { throw e
                                            } catch (e: Exception) {
                                                LogManager.addLog("ERROR", "CFG", "socket protocol change failed: ${e.message}")
                                                configFailure = true
                                                throw e
                                            } finally { savingSocketProtocols = false }
                                        }
                                    }
                                }, label = { Text(protocols.configValue.uppercase()) })
                        }
                    }
                }
            }
            /* Auto mode: read-only live socket display */
            if (localSocketStates.isEmpty()) {
                item(key = "no-sockets") { Text(stringResource(R.string.no_sockets), color = MaterialTheme.colorScheme.onSurfaceVariant) }
            } else {
                items(localSocketStates) { s ->
                    val stateColor = when (s.state) { 2 -> Color(0xFF81C784); 1 -> Color(0xFFF1C40F); else -> Color(0xFFFFB4AB) }
                    Column(modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text("${s.bindIp}:${s.port}", color = MaterialTheme.colorScheme.onSurfaceVariant, style = MaterialTheme.typography.bodySmall)
                            Spacer(Modifier.width(6.dp))
                            Surface(shape = MaterialTheme.shapes.extraSmall, color = stateColor,
                                modifier = Modifier.size(10.dp)) {}
                            Spacer(Modifier.width(4.dp))
                            Text(s.stateLabel, style = MaterialTheme.typography.labelSmall, color = stateColor)
                            Spacer(Modifier.width(6.dp))
                            Text(s.protocol, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            if (s.linksUp + s.linksDown > 0) {
                                Spacer(Modifier.width(4.dp))
                                Text("[${s.linksUp}/${s.linksDown}]", style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                        Text(s.ifName, style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.fillMaxWidth())
                        LocalSocketNatInfo(s)
                    }
                }
            }
        } else {
            /* Manual mode: existing editable server config */
            itemsIndexed(servers, key = { _, srv -> "server-${srv.id}" }) { idx, srv ->
                Column(modifier = Modifier.fillMaxWidth()) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        var ifExpanded by remember { mutableStateOf(false) }
                        ExposedDropdownMenuBox(expanded = ifExpanded, onExpandedChange = { ifExpanded = it }, modifier = Modifier.weight(1f)) {
                            OutlinedTextField(srv.ifName, {}, readOnly = true, singleLine = true,
                                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(ifExpanded) },
                                modifier = Modifier.menuAnchor().heightIn(min = 40.dp))
                            ExposedDropdownMenu(ifExpanded, onDismissRequest = { ifExpanded = false }) {
                                availableIfaces.forEach { (n, ip) ->
                                    DropdownMenuItem(text = { Text("$n ($ip)") }, onClick = {
                                        save(servers.toMutableList().also { it[idx] = it[idx].copy(ifName = n, bindIp = ip) }); ifExpanded = false
                                    })
                                }
                            }
                        }
                        Spacer(Modifier.width(4.dp))
                        Text(srv.bindIp, color = MaterialTheme.colorScheme.onSurfaceVariant, style = MaterialTheme.typography.bodySmall,
                            maxLines = 1, modifier = Modifier.width(105.dp))
                        IconButton(onClick = { save(servers.toMutableList().also { it.removeAt(idx) }) }, modifier = Modifier.size(28.dp)) {
                            Icon(Icons.Default.Close, stringResource(R.string.remove), modifier = Modifier.size(16.dp), tint = MaterialTheme.colorScheme.error)
                        }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        var protoExpanded by remember { mutableStateOf(false) }
                        ExposedDropdownMenuBox(expanded = protoExpanded, onExpandedChange = { protoExpanded = it }, modifier = Modifier.width(100.dp)) {
                            OutlinedTextField(srv.protocol.uppercase(), {}, readOnly = true, singleLine = true,
                                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(protoExpanded) },
                                modifier = Modifier.menuAnchor().heightIn(min = 40.dp))
                            ExposedDropdownMenu(protoExpanded, onDismissRequest = { protoExpanded = false }) {
                                PROTOCOLS.forEach { p ->
                                    DropdownMenuItem(text = { Text(p.uppercase()) }, onClick = {
                                        save(servers.toMutableList().also { it[idx] = srv.copy(protocol = p) }); protoExpanded = false
                                    })
                                }
                            }
                        }
                        Spacer(Modifier.width(4.dp))
                        var portText by rememberSaveable(srv.id) { mutableStateOf(srv.port.toString()) }
                        OutlinedTextField(portText, { v -> portText = v; val p = v.toIntOrNull(); if (p != null && p in 1..65535)
                                save(servers.toMutableList().also { it[idx] = srv.copy(port = p) }) },
                            singleLine = true, modifier = Modifier.width(80.dp).heightIn(min = 40.dp),
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
                    }
                    Spacer(Modifier.height(2.dp))
                }
            }
            item(key = "add-interface") {
                TextButton(onClick = {
                    val iface = availableIfaces.firstOrNull() ?: ("lo" to "127.0.0.1")
                    val rndPort = (2000..9999).random()
                    save(servers + ServerEntry(UUID.randomUUID().toString(), iface.first, iface.second, "tcp", rndPort))
                }, enabled = serversLoaded, modifier = Modifier.fillMaxWidth()) {
                    Icon(Icons.Default.Add, null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(4.dp))
                    Text(stringResource(R.string.add_interface))
                }
            }
        }

        item(key = "standby-divider") { HorizontalDivider(Modifier.padding(vertical = 8.dp)) }

        /* ── Standby (background duty-cycle) ── */
        item(key = "standby-title") { Text(stringResource(R.string.standby), style = MaterialTheme.typography.titleMedium) }
        item(key = "standby-duty-cycle") {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(standbyActiveSec,
                    { v -> standbyActiveSec = v; val nb = v.toIntOrNull(); if (nb != null && nb in 1..3600) {
                        provider.changes.submit("standby active") { provider.setValue("chat.standby_active_sec", v)
                            if (NativeLib.isReady()) NativeLib.setChatSetting("standby_active_sec", v) } } },
                    label = { Text(stringResource(R.string.standby_active_seconds)) }, modifier = Modifier.weight(1f).heightIn(min = 40.dp),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), singleLine = true)
                OutlinedTextField(standbySleepSec,
                    { v -> standbySleepSec = v; val nb = v.toIntOrNull(); if (nb != null && nb in 1..86400) {
                        provider.changes.submit("standby sleep") { provider.setValue("chat.standby_sleep_sec", v)
                            if (NativeLib.isReady()) NativeLib.setChatSetting("standby_sleep_sec", v) } } },
                    label = { Text(stringResource(R.string.standby_sleep_seconds)) }, modifier = Modifier.weight(1f).heightIn(min = 40.dp),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), singleLine = true)
            }
        }
        item(key = "standby-min-sleep") {
            OutlinedTextField(standbyMinSleepSec,
                { v -> standbyMinSleepSec = v; val nb = v.toIntOrNull(); if (nb != null && nb in 1..86400) {
                    provider.changes.submit("standby minimum sleep") { provider.setValue("chat.standby_min_sleep_sec", v)
                        if (NativeLib.isReady()) NativeLib.setChatSetting("standby_min_sleep_sec", v) } } },
                label = { Text(stringResource(R.string.standby_min_sleep_seconds)) }, modifier = Modifier.heightIn(min = 40.dp),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), singleLine = true)
        }
        item(key = "standby-keepalive") {
            OutlinedTextField(standbyKeepaliveMs,
                { v -> standbyKeepaliveMs = v; val nb = v.toIntOrNull(); if (nb != null && nb in 1000..600000) {
                    provider.changes.submit("standby keepalive") { provider.setValue("chat.standby_keepalive_ms", v)
                        if (NativeLib.isReady()) NativeLib.setChatSetting("standby_keepalive_ms", v) } } },
                label = { Text(stringResource(R.string.standby_keepalive_ms)) }, modifier = Modifier.heightIn(min = 40.dp),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), singleLine = true)
        }

        item(key = "chat-divider") { HorizontalDivider(Modifier.padding(vertical = 8.dp)) }

        /* ── Chat ── */
        item(key = "chat-title") { Text(stringResource(R.string.chat), style = MaterialTheme.typography.titleMedium) }
        item(key = "autoconnect") {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.group_autoconnect), modifier = Modifier.weight(1f))
                Switch(checked = autoConnect, onCheckedChange = {
                    val s = if (it) "1" else "0"
                    provider.changes.submit("group autoconnect") {
                        provider.setValue("chat.group_autoconnect", s); autoConnect = s == "1"
                        if (NativeLib.isReady()) NativeLib.setChatSetting("group_autoconnect", s)
                    }
                })
            }
        }

        item(key = "debug-divider") { HorizontalDivider(Modifier.padding(vertical = 8.dp)) }

        /* Debug */
        item(key = "debug-title") { Text(stringResource(R.string.debug), style = MaterialTheme.typography.titleMedium) }
        item(key = "debug-level") { DebugLevelDropdown(label = stringResource(R.string.global_level), current = debugLevel.uppercase(), onSelect = setDebug) }
        item(key = "log-udp-enabled") {
            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).toggleable(value = logUdpEnabled, enabled = logUdpLoaded,
                role = Role.Checkbox, onValueChange = { logUdpEnabled = it; saveUdpLogSettings() }),
                verticalAlignment = Alignment.CenterVertically) {
                Checkbox(checked = logUdpEnabled, onCheckedChange = null, enabled = logUdpLoaded)
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.udp_log_enabled), modifier = Modifier.weight(1f))
            }
        }
        item(key = "log-udp") {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(logUdpIp, { v ->
                    logUdpIp = v; saveUdpLogSettings()
                }, label = { Text(stringResource(R.string.udp_log_ip)) }, enabled = logUdpLoaded,
                    isError = logUdpEnabled && !UdpLogSettings(ip = logUdpIp).validAddress,
                    modifier = Modifier.weight(1f).heightIn(min = 40.dp), singleLine = true)
                OutlinedTextField(logUdpPort, { v ->
                    logUdpPort = v; saveUdpLogSettings()
                }, label = { Text(stringResource(R.string.port)) }, enabled = logUdpLoaded,
                    isError = logUdpEnabled && (logUdpPort.toIntOrNull() ?: 0) !in 1..65535,
                    modifier = Modifier.width(80.dp).heightIn(min = 40.dp),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), singleLine = true)
            }
        }
        item(key = "debug-categories-toggle") {
            Row(modifier = Modifier.fillMaxWidth().clickable { showDebugCategories = !showDebugCategories },
                horizontalArrangement = Arrangement.SpaceBetween) {
                Text(stringResource(R.string.categories), style = MaterialTheme.typography.titleMedium)
                Icon(Icons.Default.ArrowDropDown, stringResource(if (showDebugCategories) R.string.collapse else R.string.expand), modifier = Modifier.padding(end = 4.dp))
            }
        }
        if (showDebugCategories) {
            items(debugCategories, key = { "debug-$it" }) { cat ->
                val current = catLevels[cat] ?: ""
                DebugLevelDropdown(label = cat, current = current.ifEmpty { stringResource(R.string.global_level_hint) },
                    onSelect = { setCatLevel(cat, if (it == "NONE") "" else it) }, compact = true)
            }
        }
        item(key = "debug-spacer") { Spacer(Modifier.height(16.dp)) }

        /* ── Background service ── */
        item(key = "service-divider") { HorizontalDivider(Modifier.padding(vertical = 8.dp)) }
        item(key = "service-title") { Text(stringResource(R.string.service), style = MaterialTheme.typography.titleMedium) }
        item(key = "start-on-boot") {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.start_on_boot))
                    Text(stringResource(R.string.start_on_boot_hint), style = MaterialTheme.typography.bodySmall)
                }
                Switch(checked = startOnBoot, enabled = !savingStartOnBoot, onCheckedChange = { enabled ->
                    savingStartOnBoot = true
                    startOnBootError = false
                    provider.changes.submit("start on boot") {
                        try {
                            provider.setStartOnBoot(enabled)
                        } catch (e: CancellationException) {
                            throw e
                        } catch (e: Exception) {
                            LogManager.addLog("ERROR", "CONFIG", "Start on boot setting save failed: enabled=$enabled error=$e")
                            startOnBootError = true
                            throw e
                        } finally {
                            savingStartOnBoot = false
                        }
                    }
                })
            }
        }
        if (startOnBootError) {
            item(key = "boot-error") { Text(stringResource(R.string.start_on_boot_save_failed), color = MaterialTheme.colorScheme.error) }
        }
        item(key = "service-actions") {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(if (serviceEnabled) R.string.service_running else R.string.service_stopped), modifier = Modifier.weight(1f),
                    color = if (serviceEnabled) Color(0xFF81C784) else MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodyMedium)
                Button(onClick = {
                    provider.changes.submit("start core service") {
                        provider.setServiceEnabled(true)
                        checkNotNull(ContextCompat.startForegroundService(ChatApplication.instance,
                            Intent(ChatApplication.instance, HeadlessService::class.java))) { "Core service component unavailable" }
                    }
                }, enabled = !serviceEnabled) { Text(stringResource(R.string.service_start)) }
                Button(onClick = {
                    provider.changes.submit("stop core service") {
                        provider.setServiceEnabled(false)
                        ChatApplication.instance.stopService(Intent(ChatApplication.instance, HeadlessService::class.java))
                    }
                }, enabled = serviceEnabled,
                    colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)) { Text(stringResource(R.string.service_stop)) }
            }
        }

        item(key = "restart-divider") { HorizontalDivider(Modifier.padding(vertical = 8.dp)) }

        item(key = "restart") {
            Button(onClick = {
                provider.changes.submit("restart core") {
                    try {
                        if (NativeLib.isReady()) {
                            val path = provider.getString("db.path") ?: ""; val port = provider.getInt("control.port")
                            check(NativeLib.restart(path, port, provider.buildConfigText().await())) { "core restart rejected" }
                        }
                    } catch (e: CancellationException) { throw e
                    } catch (e: Exception) {
                        LogManager.addLog("ERROR", "CFG", "restart failed: ${e.javaClass.simpleName}: ${e.message}")
                        withContext(Dispatchers.Main) { configFailure = true }
                        throw e
                    }
                }
            }, modifier = Modifier.fillMaxWidth(), colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.secondary)) {
                Icon(Icons.Default.Refresh, null, modifier = Modifier.size(18.dp)); Spacer(Modifier.width(8.dp)); Text(stringResource(R.string.instance_restart))
            }
        }
        item(key = "exit-spacer") { Spacer(Modifier.height(8.dp)) }
        item(key = "exit") {
            Button(onClick = {
                val request = provider.changes.submit("stop service and exit") {
                    provider.setServiceEnabled(false)
                    ChatApplication.instance.stopService(Intent(ChatApplication.instance, HeadlessService::class.java))
                }
                scope.launch {
                    try { request.await(); onExit() }
                    catch (e: CancellationException) { throw e }
                    catch (e: Exception) { provider.changes.reportFailure("exit after service stop", e) }
                }
            }, modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)) {
                Icon(Icons.Default.PowerSettingsNew, null, modifier = Modifier.size(18.dp)); Spacer(Modifier.width(8.dp)); Text(stringResource(R.string.stop_all_exit))
            }
        }
        item(key = "bottom-spacer") { Spacer(Modifier.height(16.dp)) }
    }

    /* Dialogs */
    if (configFailure || databaseReset.failed) AlertDialog(
        onDismissRequest = { configFailure = false; vm.dismissDatabaseResetError() }, title = { Text(stringResource(R.string.settings)) },
        text = { Text(stringResource(if (databaseReset.failed) R.string.database_reset_failed else R.string.settings_update_failed)) },
        confirmButton = {
            TextButton(onClick = { configFailure = false; vm.dismissDatabaseResetError() }) { Text(stringResource(R.string.close)) }
        })
    if (clearingDb) AlertDialog(onDismissRequest = {}, title = { Text(stringResource(R.string.settings)) },
        text = { CircularProgressIndicator() }, confirmButton = {})
    if (showOverwriteKeys) AlertDialog(
        onDismissRequest = { showOverwriteKeys = false }, title = { Text(stringResource(R.string.overwrite_keys_question)) },
        text = { Text(stringResource(R.string.overwrite_keys_warning)) },
        confirmButton = { Button(onClick = { showOverwriteKeys = false; vm.requestRegenerateKeys() }) { Text(stringResource(R.string.overwrite)) } },
        dismissButton = { TextButton(onClick = { showOverwriteKeys = false }) { Text(stringResource(R.string.cancel)) } })

    if (showClearDb) AlertDialog(
        onDismissRequest = { showClearDb = false }, title = { Text(stringResource(R.string.clear_db_question)) },
        text = { Text(stringResource(R.string.clear_db_warning)) },
        confirmButton = { Button(onClick = {
            showClearDb = false
            vm.resetDatabase()
        }, enabled = !clearingDb, colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)) { Text(stringResource(R.string.delete_all)) } },
        dismissButton = { TextButton(onClick = { showClearDb = false }) { Text(stringResource(R.string.cancel)) } })
}

/* Измерители показывают фактические RMS и вероятность речи из native-детекторов. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun RadioAutomaticSettings(provider: ConfigProvider, compact: Boolean = false) {
    val engine = remember { RadioAudioEngine.instance() }
    val saved by remember { provider.changes.observe("radio automatic preferences", provider.radioAutoSettings) }
        .collectAsState(initial = remember { provider.getRadioAutoSettings() })
    var draft by remember(saved) { mutableStateOf(saved) }
    var saving by remember { mutableStateOf(false) }
    var saveError by remember { mutableStateOf(false) }
    var resumed by remember { mutableStateOf(false) }
    var level by remember { mutableStateOf<Float?>(null) }
    var vadProbability by remember { mutableStateOf<Float?>(null) }
    var vadGain by remember { mutableStateOf<Float?>(null) }
    var vadCompressorEnabled by remember { mutableStateOf(false) }
    var blocked by remember { mutableStateOf(false) }
    var active by remember { mutableStateOf(engine.isActive()) }
    var micPermission by remember { mutableStateOf(MicrophonePermission.granted()) }
    val autoActive by engine.autoActiveFlow.collectAsState()
    val transmitting by engine.transmittingGroup.collectAsState()
    LifecycleResumeEffect(engine) {
        resumed = true
        engine.setLevelMonitoring(true)
        onPauseOrDispose { resumed = false; engine.setLevelMonitoring(false) }
    }
    LaunchedEffect(resumed) {
        if (!resumed) { level = null; vadProbability = null; vadGain = null; return@LaunchedEffect }
        try { while (true) {
            val status = NativeLib.radioInputStatus()
            active = engine.isActive()
            micPermission = MicrophonePermission.granted()
            level = if (active && status[1] != 0f) status[0] else null
            vadProbability = if (active && status[4] != 0f) status[3] else null
            vadGain = if (active && status[4] != 0f) status[5] else null
            vadCompressorEnabled = status[6] != 0f
            blocked = status[2] != 0f
            delay(100)
        } } catch (e: CancellationException) { throw e }
        catch (e: Exception) {
            level = null; vadProbability = null; vadGain = null
            provider.changes.reportFailure("radio meter", e)
        }
    }
    if (!compact) Text(stringResource(R.string.radio_automatic), style = MaterialTheme.typography.titleMedium)
    if (!compact) Row(verticalAlignment = Alignment.CenterVertically) {
        Text(stringResource(R.string.radio_auto_enabled_label), Modifier.weight(1f))
        Switch(draft.enabled, { draft = draft.copy(enabled = it) }, enabled = !saving)
    }
    Text(stringResource(R.string.radio_activation_mode), style = MaterialTheme.typography.bodyMedium)
    // Прямой выбор без второго popup внутри мини-блока настроек.
    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
        listOf(RadioAutoSettings.VAD, RadioAutoSettings.LEVEL, RadioAutoSettings.VAD_LEVEL).forEachIndexed { index, mode ->
            SegmentedButton(selected = draft.mode == mode, enabled = !saving,
                shape = SegmentedButtonDefaults.itemShape(index, 3), modifier = Modifier.heightIn(min = 48.dp), onClick = {
                    LogManager.addLog("INFO", "radio", "Select automatic transmission mode: ${draft.mode} -> $mode (pending Apply)")
                    draft = draft.copy(mode = mode)
                }) {
                Text(stringResource(when (mode) {
                    RadioAutoSettings.VAD -> R.string.vad_button
                    RadioAutoSettings.LEVEL -> R.string.radio_level_mode
                    else -> R.string.radio_vad_level_mode
                }))
            }
        }
    }
    if (draft.mode == RadioAutoSettings.VAD_LEVEL)
        Text(stringResource(R.string.radio_vad_level_hint), style = MaterialTheme.typography.bodySmall)
    Text(stringResource(when {
        !active -> R.string.radio_level_enable_radio
        !micPermission -> R.string.microphone_access_required
        transmitting != null -> R.string.radio_auto_transmitting
        saved.enabled && autoActive && blocked -> R.string.radio_auto_busy
        saved.enabled && autoActive -> R.string.radio_auto_listening
        saved.enabled -> R.string.radio_auto_inactive
        else -> R.string.radio_auto_disabled
    }), style = MaterialTheme.typography.bodySmall)
    if (active && !micPermission) TextButton(onClick = {
        MicrophonePermission.request("Radio level meter") { engine.setLevelMonitoring(resumed) }
    }) { Text(stringResource(R.string.radio_level_allow_mic)) }
    Text(if (level == null) stringResource(R.string.radio_level_unavailable) else
        stringResource(R.string.radio_level_current, level!!), style = MaterialTheme.typography.bodySmall)
    val background = MaterialTheme.colorScheme.surfaceVariant
    Canvas(Modifier.fillMaxWidth().height(28.dp)) {
        fun x(db: Float) = ((db + 90f) / 90f).coerceIn(0f, 1f) * size.width
        drawRect(background)
        level?.let { drawRect(Color(0xFF27AE60), size = Size(x(it), size.height)) }
        drawLine(Color(0xFF3390EC), Offset(x(draft.levelOff.toFloat()), 0f), Offset(x(draft.levelOff.toFloat()), size.height), 3.dp.toPx())
        drawLine(Color(0xFFE67E22), Offset(x(draft.levelOn.toFloat()), 0f), Offset(x(draft.levelOn.toFloat()), size.height), 3.dp.toPx())
    }
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text("−90 dBFS", style = MaterialTheme.typography.bodySmall)
        Text("0", style = MaterialTheme.typography.bodySmall)
    }
    LabeledSlider(stringResource(R.string.radio_level_on), draft.levelOn.toFloat(), -90f..0f, 89, "${draft.levelOn} dBFS",
        { if (!saving) draft = draft.copy(levelOn = it.toInt()) })
    LabeledSlider(stringResource(R.string.radio_level_off), draft.levelOff.toFloat(), -90f..0f, 89, "${draft.levelOff} dBFS",
        { if (!saving) draft = draft.copy(levelOff = it.toInt()) })
    HorizontalDivider()
    Text(stringResource(R.string.vad_button), style = MaterialTheme.typography.titleSmall)
    Text(vadProbability?.let { stringResource(R.string.radio_vad_current, it * 100f) } ?: stringResource(when {
        !saved.enabled || !saved.usesVad -> R.string.radio_vad_enable
        blocked -> R.string.radio_vad_busy
        else -> R.string.radio_vad_unavailable
    }), style = MaterialTheme.typography.bodySmall)
    Canvas(Modifier.fillMaxWidth().height(20.dp)) {
        drawRect(background)
        vadProbability?.let { drawRect(Color(0xFF27AE60), size = Size(it.coerceIn(0f, 1f) * size.width, size.height)) }
        val thresholdX = draft.vadThreshold / 100f * size.width
        drawLine(Color(0xFFE67E22), Offset(thresholdX, 0f), Offset(thresholdX, size.height), 3.dp.toPx())
    }
    LabeledSlider(stringResource(R.string.speech_threshold), draft.vadThreshold.toFloat(), 10f..95f, 84, "${draft.vadThreshold}%",
        { if (!saving) draft = draft.copy(vadThreshold = it.toInt()) })
    Text(when {
        vadGain == null -> stringResource(R.string.radio_vad_gain_unavailable)
        !vadCompressorEnabled -> stringResource(R.string.radio_vad_compressor_off)
        else -> stringResource(R.string.radio_vad_gain_current, vadGain!!)
    }, style = MaterialTheme.typography.bodySmall)
    if (!compact) Text(stringResource(R.string.vad_compressor_hint), style = MaterialTheme.typography.bodySmall)
    LabeledSlider(stringResource(R.string.silence_delay), draft.hangoverMs.toFloat(), 50f..2000f, 38,
        stringResource(R.string.milliseconds, draft.hangoverMs), { if (!saving) draft = draft.copy(hangoverMs = it.toInt()) })
    if (draft.levelOff >= draft.levelOn) Text(stringResource(R.string.radio_level_invalid), color = MaterialTheme.colorScheme.error)
    if (saveError) Text(stringResource(R.string.radio_auto_save_failed), color = MaterialTheme.colorScheme.error)
    Button(enabled = !saving && draft.valid() && draft != saved, onClick = {
        val next = draft.copy(vadGain = provider.getVoiceSettings().vadMaxGainDb)
        val restart = next.enabled && !saved.enabled
        saving = true; saveError = false
        provider.changes.submit("radio automatic transmission") {
            try {
                provider.saveRadioAutoSettings(next)
                engine.reconfigure(restartAuto = restart)
                if (next.enabled) RadioOverlayService.requestPermissions() else RadioOverlayService.sync(ChatApplication.instance)
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                saveError = true
                LogManager.addLog("ERROR", "radio", "Cannot save automatic transmission settings: $e")
                throw e
            } finally { saving = false }
        }
    }) { Text(stringResource(R.string.radio_auto_apply)) }
    if (!compact) {
        Text(stringResource(R.string.radio_level_hint), style = MaterialTheme.typography.bodySmall)
        Text(stringResource(R.string.vad_screen_hint), style = MaterialTheme.typography.bodySmall)
    }
}

/* ── Radio Settings Tab ── */

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun RadioSettingsTab() {
    val provider = ChatApplication.instance.configProvider

    Column(
        modifier = Modifier.padding(16.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        val keepReady by remember { provider.changes.observe("radio microphone standby", provider.radioKeepMicrophoneReady) }
            .collectAsState(initial = provider.getRadioKeepMicrophoneReady())
        var keepReadySaving by remember { mutableStateOf(false) }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.radio_keep_microphone_ready), Modifier.weight(1f))
            Switch(checked = keepReady, enabled = !keepReadySaving, onCheckedChange = { enabled ->
                keepReadySaving = true
                provider.changes.submit("radio microphone standby") {
                    try {
                        provider.setRadioKeepMicrophoneReady(enabled)
                        val radio = RadioAudioEngine.instance()
                        radio.refreshMicrophonePolicy()
                        if (enabled && radio.isActive() && !MicrophonePermission.granted())
                            MicrophonePermission.request("Radio microphone standby") { radio.refreshMicrophonePolicy() }
                    } finally { keepReadySaving = false }
                }
            })
        }
        Text(stringResource(R.string.radio_keep_microphone_ready_hint), style = MaterialTheme.typography.bodySmall)
        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        RadioAutomaticSettings(provider)
        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        /* ── Volume-кнопки PTT ── */
        val volumePtt by remember { provider.changes.observe("volume PTT preference", provider.radioVolumePttEnabled) }
            .collectAsState(initial = provider.getRadioVolumePtt())
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.volume_ptt), modifier = Modifier.weight(1f))
            Switch(checked = volumePtt, onCheckedChange = {
                provider.changes.submit("volume PTT") { provider.setRadioVolumePtt(it) }
            })
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        /* ── Оверлей PTT поверх приложений ── */
        val overlayEnabled by remember { provider.changes.observe("overlay preference", provider.radioOverlayEnabled) }
            .collectAsState(initial = provider.getRadioOverlay())
        val overlayShown by RadioOverlayService.isShown.collectAsState()
        val overlayStatus by RadioOverlayService.status.collectAsState()
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.ptt_overlay))
                if (overlayEnabled) Text(
                    overlayStatus?.let { stringResource(it) } ?: stringResource(if (overlayShown) R.string.overlay_active else R.string.waiting_enable),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.clickable { RadioOverlayService.requestPermissions() }
                )
            }
            Switch(checked = overlayEnabled, onCheckedChange = { enabled ->
                provider.changes.submit("radio overlay") { RadioOverlayService.setEnabled(enabled) }
            })
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        /* ── Источник микрофона ── */
        val micSources = listOf(
            stringResource(R.string.mic_source_voice), stringResource(R.string.mic_source_mic),
            stringResource(R.string.mic_source_raw), stringResource(R.string.mic_source_camera))
        var micSource by remember { mutableStateOf(provider.getRadioMicSource().coerceIn(0, micSources.size - 1)) }
        var micExpanded by remember { mutableStateOf(false) }
        Text(stringResource(R.string.microphone_source), style = MaterialTheme.typography.titleMedium)
        ExposedDropdownMenuBox(expanded = micExpanded, onExpandedChange = { micExpanded = it }) {
            OutlinedTextField(micSources[micSource], {}, readOnly = true, singleLine = true,
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(micExpanded) },
                modifier = Modifier.menuAnchor().heightIn(min = 40.dp))
            ExposedDropdownMenu(micExpanded, onDismissRequest = { micExpanded = false }) {
                micSources.forEachIndexed { i, n ->
                    DropdownMenuItem(text = { Text(n) }, onClick = {
                        provider.changes.submit("radio microphone source") {
                            provider.setRadioMicSource(i)
                            micSource = i
                            RadioAudioEngine.instance().reconfigure()
                        }
                        micExpanded = false
                    })
                }
            }
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        /* ── Захват стерео ── */
        var captureStereo by remember { mutableStateOf(provider.getRadioCaptureStereo()) }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.stereo_capture))
                Text(stringResource(R.string.stereo_playback_hint),
                    style = MaterialTheme.typography.bodySmall)
            }
            Switch(checked = captureStereo, onCheckedChange = {
                provider.changes.submit("radio stereo capture") {
                    provider.setRadioCaptureStereo(it)
                    captureStereo = it
                    NativeLib.setChatSetting("radio_capture_stereo", if (it) "1" else "0")
                    RadioAudioEngine.instance().reconfigure()
                }
            })
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        var radioAecOn by remember { mutableStateOf(provider.getInt("radio.aec_enabled") != 0) }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.radio_aec_enabled), modifier = Modifier.weight(1f))
            Switch(checked = radioAecOn, onCheckedChange = { enabled ->
                provider.changes.submit("radio echo cancellation") {
                    provider.setValue("radio.aec_enabled", if (enabled) "1" else "0")
                    if (NativeLib.isReady()) NativeLib.setChatSetting("radio_aec_enabled", if (enabled) "1" else "0")
                    RadioAudioEngine.instance().reconfigure()
                    radioAecOn = enabled
                    LogManager.addLog("INFO", "radio", "Echo cancellation requested enabled=$enabled")
                }
            })
        }

        /* ── Компрессор (AGC) рации ── */
        var radioCompOn by remember { mutableStateOf(provider.getInt("radio.compressor_enabled") != 0) }
        var radioCompMaxGain by remember { mutableStateOf(provider.getInt("radio.compressor_max_gain_db")) }
        Text(stringResource(R.string.radio_compressor), style = MaterialTheme.typography.titleMedium)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.enable_compressor), modifier = Modifier.weight(1f))
            Switch(checked = radioCompOn, onCheckedChange = {
                provider.changes.submit("radio compressor") {
                    provider.setValue("radio.compressor_enabled", if (it) "1" else "0"); radioCompOn = it
                    if (NativeLib.isReady()) NativeLib.setChatSetting("radio_compressor_enabled", if (it) "1" else "0")
                    RadioAudioEngine.instance().reconfigure()
                }
            })
        }
        LabeledSlider(stringResource(R.string.max_gain), radioCompMaxGain.toFloat(), 1f..45f, 44, "${radioCompMaxGain} dB",
            { v ->
                radioCompMaxGain = v.toInt()
            },
            onValueChangeFinished = {
                val gain = radioCompMaxGain
                provider.changes.submit("radio compressor gain") {
                    provider.setValue("radio.compressor_max_gain_db", gain.toString())
                    if (NativeLib.isReady()) NativeLib.setChatSetting("radio_compressor_max_gain_db", gain.toString())
                    RadioAudioEngine.instance().reconfigure()
                }
            })

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        /* ── Opus для рации: режим + битрейт ── */
        val opusModes = listOf(stringResource(R.string.opus_voice), stringResource(R.string.opus_music))
        val opusBitrates = listOf(16, 32, 64, 96, 128)
        var opusMode by remember { mutableStateOf(provider.getInt("radio.opus_mode").coerceIn(0, 1)) }
        var opusModeExpanded by remember { mutableStateOf(false) }
        var opusBitrate by remember { mutableStateOf(provider.getInt("radio.opus_bitrate")) }
        var opusBitrateExpanded by remember { mutableStateOf(false) }
        Text(stringResource(R.string.radio_opus), style = MaterialTheme.typography.titleMedium)
        var burstFrames by remember { mutableIntStateOf(provider.getInt("radio.burst_frames")) }
        var burstExpanded by remember { mutableStateOf(false) }
        ExposedDropdownMenuBox(expanded = burstExpanded, onExpandedChange = { burstExpanded = it }) {
            OutlinedTextField(burstFrames.toString(), {}, readOnly = true, singleLine = true,
                label = { Text(stringResource(R.string.radio_burst_frames)) },
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(burstExpanded) },
                modifier = Modifier.menuAnchor().heightIn(min = 40.dp))
            ExposedDropdownMenu(burstExpanded, onDismissRequest = { burstExpanded = false }) {
                (1..10).forEach { frames ->
                    DropdownMenuItem(text = { Text(frames.toString()) }, onClick = {
                        provider.changes.submit("radio burst frames") {
                            provider.setValue("radio.burst_frames", frames.toString())
                            if (NativeLib.isReady()) NativeLib.setChatSetting("radio_burst_frames", frames.toString())
                            burstFrames = frames
                        }
                        burstExpanded = false
                    })
                }
            }
        }
        Text(stringResource(R.string.radio_burst_hint), style = MaterialTheme.typography.bodySmall)
        ExposedDropdownMenuBox(expanded = opusModeExpanded, onExpandedChange = { opusModeExpanded = it }) {
            OutlinedTextField(opusModes[opusMode], {}, readOnly = true, singleLine = true,
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(opusModeExpanded) },
                modifier = Modifier.menuAnchor().heightIn(min = 40.dp))
            ExposedDropdownMenu(opusModeExpanded, onDismissRequest = { opusModeExpanded = false }) {
                opusModes.forEachIndexed { i, n ->
                    DropdownMenuItem(text = { Text(n) }, onClick = {
                        provider.changes.submit("radio Opus mode") {
                            provider.setValue("radio.opus_mode", i.toString()); opusMode = i
                            if (NativeLib.isReady()) NativeLib.setChatSetting("radio_opus_mode", i.toString())
                            RadioAudioEngine.instance().reconfigure()
                        }
                        opusModeExpanded = false
                    })
                }
            }
        }
        ExposedDropdownMenuBox(expanded = opusBitrateExpanded, onExpandedChange = { opusBitrateExpanded = it }) {
            OutlinedTextField(stringResource(R.string.bitrate_kbps, opusBitrate), {}, readOnly = true, singleLine = true,
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(opusBitrateExpanded) },
                modifier = Modifier.menuAnchor().heightIn(min = 40.dp))
            ExposedDropdownMenu(opusBitrateExpanded, onDismissRequest = { opusBitrateExpanded = false }) {
                opusBitrates.forEach { b ->
                    DropdownMenuItem(text = { Text(stringResource(R.string.bitrate_kbps, b)) }, onClick = {
                        provider.changes.submit("radio Opus bitrate") {
                            provider.setValue("radio.opus_bitrate", b.toString()); opusBitrate = b
                            if (NativeLib.isReady()) NativeLib.setChatSetting("radio_opus_bitrate", b.toString())
                            RadioAudioEngine.instance().reconfigure()
                        }
                        opusBitrateExpanded = false
                    })
                }
            }
        }
    }
}

/* ── Audio Settings Tab ── */

private data class AudioSettingsSnapshot(val voice: VoiceSettings, val bitrate: CallVideoBitrateRange,
                                         val jitterMs: Int, val sound: Boolean, val proximity: Pair<Boolean, Float>,
                                         val noise: NoiseSuppressionSettings)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AudioSettingsTab(vm: ChatViewModel) {
    val provider = ChatApplication.instance.configProvider
    var snapshot by remember { mutableStateOf<AudioSettingsSnapshot?>(null) }
    var loadFailed by remember { mutableStateOf(false) }
    var loadAttempt by remember { mutableIntStateOf(0) }
    LaunchedEffect(loadAttempt) {
        loadFailed = false
        try {
            snapshot = withContext(Dispatchers.IO) { AudioSettingsSnapshot(provider.getVoiceSettings(), provider.getCallVideoBitrateRange(),
                provider.getInt("chat.call_jitter_max_reserve_ms").coerceIn(500, 2000), provider.getSoundEnabled(),
                provider.proximitySettings.first(), provider.getNoiseSuppressionSettings()) }
        } catch (e: CancellationException) { throw e }
        catch (e: Exception) { loadFailed = true; provider.changes.reportFailure("load audio settings", e) }
    }
    val settingsSnapshot = snapshot
    if (settingsSnapshot == null) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if (loadFailed) {
                Text(stringResource(R.string.settings_update_failed), color = MaterialTheme.colorScheme.error)
                Button(onClick = { loadAttempt++ }) { Text(stringResource(R.string.retry)) }
            } else CircularProgressIndicator()
        }
        return
    }

    /* Mic test state */
    var micTesting by remember { mutableStateOf(false) }
    var micLevel by remember { mutableStateOf(0f) }
    var micTestFailed by remember { mutableStateOf(false) }
    var open by remember { mutableStateOf(true) }
    DisposableEffect(vm.audioRecorder) {
        onDispose { open = false; vm.audioRecorder.requestMicTestStop() }
    }

    /* Notification sound */
    var soundEnabled by remember { mutableStateOf(settingsSnapshot.sound) }
    val proximitySettings by remember { provider.changes.observe("proximity settings", provider.proximitySettings) }
        .collectAsState(initial = settingsSnapshot.proximity)
    var proximityThreshold by remember { mutableFloatStateOf(proximitySettings.second) }
    LaunchedEffect(proximitySettings.second) { proximityThreshold = proximitySettings.second }

    LaunchedEffect(micTesting) {
        try { while (micTesting && vm.audioRecorder.isTesting) { micLevel = NativeLib.voiceGetPeakLevel(); delay(25) } }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { micTestFailed = true; provider.changes.reportFailure("microphone meter", e) }
        micTesting = false; micLevel = 0f
    }

    /* Codec preset */
    val presets = listOf(stringResource(R.string.codec_low), stringResource(R.string.codec_standard), stringResource(R.string.codec_high))
    val savedVoiceSettings = settingsSnapshot.voice
    var preset by remember { mutableStateOf(savedVoiceSettings.preset) }
    var presetExpanded by remember { mutableStateOf(false) }

    var jitterReserveMs by remember { mutableStateOf(settingsSnapshot.jitterMs) }
    val savedVideoRange = settingsSnapshot.bitrate
    var videoMinimum by rememberSaveable { mutableIntStateOf(savedVideoRange.minimum) }
    var videoMaximum by rememberSaveable { mutableIntStateOf(savedVideoRange.maximum) }
    var videoRangeSaving by remember { mutableStateOf(false) }
    var videoRangeSaveFailed by remember { mutableStateOf(false) }

    /* Все параметры восстанавливаются из одного снимка DataStore. */
    var compressorOn by remember { mutableStateOf(savedVoiceSettings.compressorEnabled) }
    var maxGainDb by remember { mutableStateOf(savedVoiceSettings.maxGainDb) }
    var lookbackMs by remember { mutableStateOf(savedVoiceSettings.lookbackMs) }
    var lookaheadMs by remember { mutableStateOf(savedVoiceSettings.lookaheadMs) }
    var riseRate by remember { mutableStateOf(savedVoiceSettings.riseRate) }
    var targetLevelDb by remember { mutableStateOf(savedVoiceSettings.targetLevelDb) }
    var gainOnlyOnSpeech by remember { mutableStateOf(savedVoiceSettings.gainOnlyOnSpeech) }
    var vadMaxGainDb by remember { mutableStateOf(savedVoiceSettings.vadMaxGainDb) }
    var noiseSettings by remember { mutableStateOf(settingsSnapshot.noise) }
    var noiseDb by remember { mutableIntStateOf(settingsSnapshot.noise.suppressDb) }
    var noiseSaving by remember { mutableStateOf(false) }
    val applyNoise = { enabled: Boolean, db: Int ->
        val requested = NoiseSuppressionSettings(enabled, db)
        noiseSaving = true
        provider.changes.submit("noise suppression") {
            try {
                provider.saveNoiseSuppressionSettings(requested)
                noiseSettings = requested
                check(withContext(Dispatchers.IO) { NativeLib.setNoiseSuppression(requested) }) {
                    "Native noise suppression apply failed: $requested"
                }
                if (NativeLib.isReady()) {
                    NativeLib.setChatSetting("noise_suppression_enabled", if (requested.enabled) "1" else "0")
                    NativeLib.setChatSetting("noise_suppression_db", requested.suppressDb.toString())
                }
            } finally { noiseDb = noiseSettings.suppressDb; noiseSaving = false }
        }
    }

    val apply = {
        val settings = VoiceSettings(preset, compressorOn, maxGainDb, lookbackMs, lookaheadMs, riseRate, targetLevelDb,
            gainOnlyOnSpeech, vadMaxGainDb)
        provider.changes.submit("voice parameters") {
            provider.saveVoiceSettings(settings)
            vm.setVoicePreset(settings.preset)
            vm.setVoiceCompressor(settings.compressorEnabled)
            vm.setCompressorConfig(settings.maxGainDb, settings.lookbackMs, settings.lookaheadMs,
                settings.riseRate.toFloat(), settings.targetLevelDb.toFloat(), settings.gainOnlyOnSpeech, settings.vadMaxGainDb)
            if (NativeLib.isReady()) {
                NativeLib.setChatSetting("opus_codec_preset", settings.preset.toString())
                NativeLib.setChatSetting("compressor_enabled", if (settings.compressorEnabled) "1" else "0")
                NativeLib.setChatSetting("compressor_max_gain_db", settings.maxGainDb.toString())
                NativeLib.setChatSetting("compressor_rise_rate", settings.riseRate.toString())
                NativeLib.setChatSetting("radio_vad_compressor_max_gain_db", settings.vadMaxGainDb.toString())
            }
        }
    }

    Column(
        modifier = Modifier.padding(16.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(stringResource(R.string.noise_suppression), style = MaterialTheme.typography.titleMedium)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.noise_suppression_enable), modifier = Modifier.weight(1f))
            Switch(checked = noiseSettings.enabled, enabled = !noiseSaving,
                onCheckedChange = { applyNoise(it, noiseDb) })
        }
        Text(stringResource(R.string.noise_suppression_hint), style = MaterialTheme.typography.bodySmall)
        LabeledSlider(stringResource(R.string.noise_suppression_strength), noiseDb.toFloat(), -40f..-1f, 38, "${noiseDb} dB",
            { noiseDb = it.roundToInt() }, { applyNoise(noiseSettings.enabled, noiseDb) }, enabled = noiseSettings.enabled && !noiseSaving)
        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        /* ── Notification sound ── */
        Text(stringResource(R.string.notification_sound), style = MaterialTheme.typography.titleMedium)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.incoming_message_sound), modifier = Modifier.weight(1f))
            Switch(checked = soundEnabled, onCheckedChange = {
                provider.changes.submit("notification sound") {
                    provider.setSoundEnabled(it); soundEnabled = it; NotificationSound.enabled = it
                }
            })
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.use_proximity_sensor), modifier = Modifier.weight(1f))
            Switch(checked = proximitySettings.first, onCheckedChange = { enabled ->
                provider.changes.submit("proximity enabled") { provider.setProximityEnabled(enabled) }
            })
        }
        Text(stringResource(R.string.proximity_threshold, proximityThreshold))
        Slider(
            value = proximityThreshold,
            onValueChange = { proximityThreshold = (it * 10).roundToInt() / 10f },
            valueRange = 0.1f..10f,
            steps = 98,
            enabled = proximitySettings.first,
            onValueChangeFinished = {
                val threshold = proximityThreshold
                provider.changes.submit("proximity threshold") {
                    try { provider.setProximityThreshold(threshold) }
                    finally { proximityThreshold = proximitySettings.second }
                }
            }
        )
        Text(stringResource(R.string.proximity_sensor_hint), style = MaterialTheme.typography.bodySmall)

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        /* ── Входящие звонки поверх блокировки ── */
        Text(stringResource(R.string.incoming_calls), style = MaterialTheme.typography.titleMedium)
        val phoneOptions by remember { provider.changes.observe("incoming call preferences", provider.incomingCallOptions) }
            .collectAsState(initial = List(5) { false })
        var requestedPhoneOption by rememberSaveable { mutableStateOf<Int?>(null) }
        var phonePermission by remember { mutableStateOf(PhoneStateMonitor.granted()) }
        val phonePermissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            phonePermission = granted
            LogManager.addLog(if (granted) "INFO" else "WARN", "PhoneState", "Phone permission result granted=$granted")
            val option = requestedPhoneOption
            requestedPhoneOption = null
            if (granted && option != null) provider.changes.submit("phone permission option") {
                provider.setIncomingCallOption(option, true)
            }
        }
        LifecycleResumeEffect(Unit) {
            phonePermission = PhoneStateMonitor.granted()
            onPauseOrDispose { }
        }
        val phoneTitles = listOf(R.string.phone_status_setting, R.string.phone_notify_setting, R.string.phone_reject_setting,
            R.string.block_incoming_calls, R.string.block_incoming_radio)
        val phoneDescriptions = listOf(R.string.phone_status_description, R.string.phone_notify_description,
            R.string.phone_reject_description, R.string.block_incoming_calls_description, R.string.block_incoming_radio_description)
        for (option in phoneTitles.indices) Row(verticalAlignment = Alignment.CenterVertically) {
            val selected = phoneOptions[option]
            val needsPermission = option < 3
            Column(Modifier.weight(1f)) {
                Text(stringResource(phoneTitles[option]))
                Text(stringResource(if (selected && needsPermission && !phonePermission) R.string.phone_status_permission_missing
                    else phoneDescriptions[option]),
                    style = MaterialTheme.typography.bodySmall)
            }
            Switch(checked = selected, enabled = requestedPhoneOption == null, onCheckedChange = { value ->
                if (value && needsPermission && !PhoneStateMonitor.granted()) {
                    requestedPhoneOption = option
                    try { phonePermissionLauncher.launch(Manifest.permission.READ_PHONE_STATE) }
                    catch (e: Exception) { requestedPhoneOption = null; provider.changes.reportFailure("phone permission launch", e) }
                } else provider.changes.submit("incoming call option") {
                    provider.setIncomingCallOption(option, value)
                }
            })
        }
        var fsiEnabled by remember { mutableStateOf(IncomingCallManager.canFullScreenIntent()) }
        LifecycleResumeEffect(Unit) {
            fsiEnabled = IncomingCallManager.canFullScreenIntent()
            onPauseOrDispose { }
        }
        Row(
            modifier = Modifier.fillMaxWidth().clickable(
                enabled = Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && !fsiEnabled
            ) {
                val ctx = ChatApplication.instance
                try {
                    val i = Intent(Settings.ACTION_MANAGE_APP_USE_FULL_SCREEN_INTENT,
                        Uri.parse("package:${ctx.packageName}"))
                    i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    ctx.startActivity(i)
                } catch (e: Exception) {
                    provider.changes.reportFailure("open full screen intent settings", e)
                }
            },
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.show_over_lock_screen))
                Text(
                    when {
                        Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE -> stringResource(R.string.always_allowed)
                        fsiEnabled -> stringResource(R.string.allowed)
                        else -> stringResource(R.string.disabled_tap_enable)
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = if (fsiEnabled) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.error
                )
            }
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        /* ── Microphone test ── */
        Text(stringResource(R.string.microphone_test), style = MaterialTheme.typography.titleMedium)
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            val lvlPct = micLevel.coerceIn(0f, 1f)
            LinearProgressIndicator(
                progress = { lvlPct },
                modifier = Modifier.weight(1f).height(16.dp),
                color = when { lvlPct < 0.6f -> Color(0xFF81C784); lvlPct < 0.85f -> Color(0xFFF1C40F); else -> Color(0xFFFFB4AB) },
                trackColor = MaterialTheme.colorScheme.surfaceVariant,
            )
            Button(onClick = {
                micTestFailed = false
                if (micTesting) { vm.audioRecorder.requestMicTestStop(); micTesting = false; micLevel = 0f }
                else {
                    try {
                        MicrophonePermission.request("microphone test") {
                            if (open) {
                                micTesting = vm.audioRecorder.startMicTest()
                                micTestFailed = !micTesting; micLevel = 0f
                                if (micTesting) apply()
                            }
                        }
                    } catch (e: Exception) { micTestFailed = true; provider.changes.reportFailure("microphone test permission", e) }
                }
            }) { Text(stringResource(if (micTesting) R.string.stop else R.string.test)) }
        }
        if (micTestFailed) Text(stringResource(R.string.media_audio_failed), color = MaterialTheme.colorScheme.error)

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        /* ── Voice codec quality ── */
        Text(stringResource(R.string.voice_codec_quality), style = MaterialTheme.typography.titleMedium)
        ExposedDropdownMenuBox(expanded = presetExpanded, onExpandedChange = { presetExpanded = it }) {
            OutlinedTextField(presets[preset], {}, readOnly = true, singleLine = true,
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(presetExpanded) },
                modifier = Modifier.menuAnchor().heightIn(min = 40.dp))
            ExposedDropdownMenu(presetExpanded, onDismissRequest = { presetExpanded = false }) {
                presets.forEachIndexed { i, n ->
                    DropdownMenuItem(text = { Text(n) }, onClick = { preset = i; apply(); presetExpanded = false })
                }
            }
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        Text(stringResource(R.string.call_max_jitter_reserve), style = MaterialTheme.typography.titleMedium)
        Text(stringResource(R.string.call_jitter_hint, "%.1f".format(jitterReserveMs / 1000.0)))
        Slider(
            value = jitterReserveMs.toFloat(),
            onValueChange = { jitterReserveMs = (it / 100).toInt() * 100 },
            valueRange = 500f..2000f,
            steps = 14,
            onValueChangeFinished = {
                val reserve = jitterReserveMs
                provider.changes.submit("call jitter reserve") {
                    provider.setValue("chat.call_jitter_max_reserve_ms", reserve.toString())
                    if (NativeLib.isReady()) NativeLib.setChatSetting("call_jitter_max_reserve_ms", reserve.toString())
                }
            }
        )

        Text(stringResource(R.string.call_video_bitrate_range), style = MaterialTheme.typography.titleMedium)
        Text(stringResource(R.string.call_video_bitrate_range_value, videoMinimum, videoMaximum))
        Text(stringResource(R.string.call_video_bitrate_range_hint), style = MaterialTheme.typography.bodySmall)
        RangeSlider(
            value = videoMinimum.toFloat()..videoMaximum.toFloat(),
            onValueChange = { videoMinimum = it.start.roundToInt(); videoMaximum = it.endInclusive.roundToInt() },
            valueRange = 300f..4000f,
            steps = 36,
            enabled = !videoRangeSaving,
            onValueChangeFinished = {
                val range = CallVideoBitrateRange(videoMinimum, videoMaximum)
                videoRangeSaving = true
                videoRangeSaveFailed = false
                val request = provider.saveCallVideoBitrateRange(range)
                provider.changes.submit("call video bitrate") {
                    try {
                        request.await()
                        if (NativeLib.isReady()) NativeLib.setChatSetting("call_video_bitrate_range_kbps", range.encode())
                    } catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        videoRangeSaveFailed = true
                        LogManager.addLog("ERROR", "CallSettings", "Video bitrate range save failed: $range: $e")
                        throw e
                    } finally {
                        videoRangeSaving = false
                    }
                }
            }
        )
        if (videoRangeSaveFailed) Text(stringResource(R.string.call_video_bitrate_range_save_failed), color = MaterialTheme.colorScheme.error)

        /* ── Voice Compressor ── */
        Text(stringResource(R.string.voice_compressor), style = MaterialTheme.typography.titleMedium)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.enable_compressor), modifier = Modifier.weight(1f))
            Switch(checked = compressorOn, onCheckedChange = { compressorOn = it; apply() })
        }

        LabeledSlider(stringResource(R.string.max_gain), maxGainDb.toFloat(), 0f..30f, 30, "${maxGainDb} dB",
            { maxGainDb = it.toInt() }, { apply() })
        LabeledSlider(stringResource(R.string.lookback_time), lookbackMs.toFloat(), 50f..500f, 45, stringResource(R.string.milliseconds, lookbackMs),
            { lookbackMs = it.toInt() }, { apply() })
        LabeledSlider(stringResource(R.string.lookahead_time), lookaheadMs.toFloat(), 20f..500f, 48, stringResource(R.string.milliseconds, lookaheadMs),
            { lookaheadMs = it.toInt() }, { apply() })
        LabeledSlider(stringResource(R.string.rise_rate), riseRate.toFloat(), 2f..10f, 8, "${riseRate} dB/s",
            { riseRate = it.toInt() }, { apply() })
        HorizontalDivider()
        Text(stringResource(R.string.shared_compressor_settings), style = MaterialTheme.typography.titleMedium)
        Text(stringResource(R.string.shared_compressor_settings_hint), style = MaterialTheme.typography.bodySmall)
        LabeledSlider(stringResource(R.string.target_level), targetLevelDb.toFloat(), -40f..0f, 40, "${targetLevelDb} dBFS",
            { targetLevelDb = it.toInt() }, { apply() })
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.compressor_gain_only_on_speech), Modifier.weight(1f))
            Switch(checked = gainOnlyOnSpeech, onCheckedChange = { gainOnlyOnSpeech = it; apply() })
        }
        Text(stringResource(R.string.compressor_gain_only_on_speech_hint), style = MaterialTheme.typography.bodySmall)
        LabeledSlider(stringResource(R.string.vad_max_gain), vadMaxGainDb.toFloat(), 1f..60f, 58, "$vadMaxGainDb dB",
            { vadMaxGainDb = it.toInt() }, { apply() })
        Text(stringResource(R.string.shared_vad_compressor_hint), style = MaterialTheme.typography.bodySmall)
    }
}

/* ── Storage Settings Tab ── */

@Composable
private fun StorageSettingsTab() {
    val provider = ChatApplication.instance.configProvider

    var autoload by remember { mutableStateOf(true) }
    var autoloadMaxSizeMb by remember { mutableStateOf("100") }
    var maxStorageGb by remember { mutableStateOf("1") }

    LaunchedEffect(Unit) {
        try {
            autoload = provider.getInt("chat.storage_autoload") != 0
            autoloadMaxSizeMb = provider.getInt("chat.storage_autoload_maxsize_mb").toString()
            maxStorageGb = provider.getInt("chat.storage_maxsize_gb").toString()
        } catch (e: CancellationException) { throw e }
        catch (e: Exception) { provider.changes.reportFailure("load storage settings", e) }
    }

    Column(
        modifier = Modifier.padding(16.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(stringResource(R.string.media_storage), style = MaterialTheme.typography.titleMedium)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.autoload_media), modifier = Modifier.weight(1f))
            Switch(checked = autoload, onCheckedChange = {
                val s = if (it) "1" else "0"
                provider.changes.submit("storage autoload") {
                    provider.setValue("chat.storage_autoload", s); autoload = s == "1"
                    if (NativeLib.isReady()) NativeLib.setChatSetting("storage_autoload", s)
                }
            })
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))

        OutlinedTextField(autoloadMaxSizeMb,
            { v ->
                autoloadMaxSizeMb = v
                val size = v.toIntOrNull()
                if (size != null && size in 1..500) provider.changes.submit("storage unit limit") {
                    provider.setValue("chat.storage_autoload_maxsize_mb", v)
                    LogManager.addLog("INFO", "CFG", "Storage unit limit saved: ${v}M; applies on next core start")
                }
            },
            label = { Text(stringResource(R.string.autoload_max_size)) },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            singleLine = true, modifier = Modifier.padding(top = 4.dp))

        OutlinedTextField(maxStorageGb,
            { v ->
                maxStorageGb = v
                val size = v.toIntOrNull()
                if (size != null && size in 1..100) provider.changes.submit("storage total limit") {
                    provider.setValue("chat.storage_maxsize_gb", v)
                    LogManager.addLog("INFO", "CFG", "Storage total limit saved: ${v}G; applies on next core start")
                }
            },
            label = { Text(stringResource(R.string.max_storage_size)) },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            singleLine = true)
        Text(stringResource(R.string.storage_limits_restart_hint), style = MaterialTheme.typography.bodySmall)
    }
}

/* ── Labeled Slider ── */

@Composable
private fun LabeledSlider(
    label: String, value: Float, range: ClosedFloatingPointRange<Float>,
    steps: Int, displayValue: String, onValueChange: (Float) -> Unit,
    onValueChangeFinished: (() -> Unit)? = null, enabled: Boolean = true
) {
    Column {
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(label, modifier = Modifier.weight(1f).padding(end = 8.dp), style = MaterialTheme.typography.bodyMedium)
            Text(displayValue, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Slider(
            value = value, onValueChange = onValueChange,
            onValueChangeFinished = onValueChangeFinished,
            valueRange = range, steps = steps,
            enabled = enabled,
            modifier = Modifier.fillMaxWidth()
        )
    }
}

/* ── Debug Level Dropdown ── */

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DebugLevelDropdown(label: String, current: String, onSelect: (String) -> Unit, compact: Boolean = false) {
    var expanded by remember { mutableStateOf(false) }
    if (compact) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, modifier = Modifier.width(120.dp), style = MaterialTheme.typography.labelMedium)
            ExposedDropdownMenuBox(expanded = expanded, onExpandedChange = { expanded = it }, modifier = Modifier.weight(1f)) {
                OutlinedTextField(current, {}, readOnly = true,
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded) },
                    modifier = Modifier.menuAnchor().fillMaxWidth().heightIn(min = 56.dp), singleLine = true)
                ExposedDropdownMenu(expanded, onDismissRequest = { expanded = false }) {
                    LEVELS.forEach { lvl ->
                        DropdownMenuItem(text = { Text(lvl) }, onClick = { onSelect(lvl); expanded = false })
                    }
                }
            }
        }
    } else {
        ExposedDropdownMenuBox(expanded = expanded, onExpandedChange = { expanded = it }, modifier = Modifier.fillMaxWidth()) {
            OutlinedTextField(current, {}, readOnly = true, label = { Text(stringResource(R.string.debug_level_label, label)) },
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded) },
                modifier = Modifier.menuAnchor().fillMaxWidth().heightIn(min = 56.dp), singleLine = true)
            ExposedDropdownMenu(expanded, onDismissRequest = { expanded = false }) {
                LEVELS.forEach { lvl ->
                    DropdownMenuItem(text = { Text(lvl) }, onClick = { onSelect(lvl); expanded = false })
                }
            }
        }
    }
}
