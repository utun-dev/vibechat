package com.utun.chat

import android.app.Application
import android.content.Intent
import androidx.core.content.ContextCompat
import android.content.res.Configuration
import android.os.Handler
import android.os.Looper
import com.utun.chat.data.LogManager
import com.utun.chat.data.ChatMessageEvent
import com.utun.chat.data.ConfigProvider
import com.utun.chat.data.NativeLib
import com.utun.chat.data.NotificationSound
import com.utun.chat.data.IncomingCallManager
import com.utun.chat.data.TelecomCallManager
import com.utun.chat.data.ChatEventCallback
import com.utun.chat.data.CallController
import com.utun.chat.data.CallVideoCodec
import com.utun.chat.data.AvatarRepository
import com.utun.chat.data.AvatarProtocol
import com.utun.chat.data.MemberRepository
import com.utun.chat.data.MemberFilesRepository
import com.utun.chat.headless.HeadlessService
import com.utun.chat.data.AppLanguage
import com.utun.chat.data.StartupIssue
import com.utun.chat.data.StartupFailure
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.CancellationException

/** Один app-scoped callback: независимые владельцы получают событие на main, затем активные подписчики SharedFlow.
 * Replay отсутствует: новая ViewModel запрашивает снимки, прежние команды/ACK ей не переигрываются. */
object AppEventHandler : ChatEventCallback {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val _events = MutableSharedFlow<Pair<Int, ByteArray?>>(extraBufferCapacity = 64)
    private val eventScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val events: SharedFlow<Pair<Int, ByteArray?>> = _events
    private val _startupFailure = MutableStateFlow<StartupFailure?>(null)
    val startupFailure = _startupFailure.asStateFlow() // Сохраняется и при запуске без Activity.

    /** Вызывается только явной кнопкой диалога. Принятая операция переживает закрытие Activity. */
    fun recreateStartupDatabase(afterStop: () -> Unit) {
        val failure = _startupFailure.value ?: return
        if (!failure.canRecreate || failure.recreating) return
        var pending = failure.copy(recreating = true, resetFailed = false, recoveryError = null)
        _startupFailure.value = pending
        val app = ChatApplication.instance
        app.configProvider.changes.submit("recreate startup database") {
            try {
                LogManager.addLog("INFO", "Startup", "User requested database recreation directory=${failure.directory}")
                HeadlessService.stopAndJoin()
                val stopped = { app.members.onEvent(13, null); app.avatars.onEvent(13, null); afterStop() }
                NativeLib.resetDatabase(failure.directory, 9999, null, stopped)
                pending = pending.copy(issue = StartupIssue.ERROR, message = AppLanguage.text(R.string.startup_database_cleared))
                _startupFailure.value = pending
                if (withContext(Dispatchers.IO) { app.configProvider.getServiceEnabled() }) {
                    checkNotNull(ContextCompat.startForegroundService(app, Intent(app, HeadlessService::class.java)))
                    LogManager.addLog("INFO", "Startup", "Database recreated; waiting for service startup")
                } else {
                    _startupFailure.value = null
                    LogManager.addLog("INFO", "Startup", "Database cleared with service disabled")
                }
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                _startupFailure.value = pending.copy(recreating = false, resetFailed = pending.canRecreate,
                    recoveryError = e.message ?: e.javaClass.simpleName)
                LogManager.addLog("ERROR", "Startup", "Database recovery failed: ${e.javaClass.simpleName}: ${e.message}")
                throw e
            }
        }
    }

    /** Отказ foreground/таймер/подготовка конфига. Событие native могло уже передать более точную причину. */
    fun serviceFailed(error: Exception) {
        val current = _startupFailure.value
        val detail = error.message ?: error.javaClass.simpleName
        if (current == null) _startupFailure.value = StartupFailure(detail, "", StartupIssue.ERROR)
        else if (current.recreating) _startupFailure.value = current.copy(recreating = false, recoveryError = detail)
    }

    /** Main: один запрос на процесс, включая поворот Activity и время между startForegroundService и SERVICE_STARTED. */
    fun recoverStartup(fromSettings: Boolean = false) {
        var pending = StartupFailure.beginRecovery(_startupFailure.value, fromSettings) ?: return
        _startupFailure.value = pending
        val app = ChatApplication.instance
        val provider = app.configProvider
        provider.changes.submit("recover chat service") {
            try {
                HeadlessService.stopAndJoin()
                val directory = withContext(Dispatchers.IO) { provider.getString("db.path") ?: app.filesDir.absolutePath }
                check(pending.directory.isEmpty() || pending.directory == directory) { "Active profile directory changed" }
                if (pending.issue == StartupIssue.INVALID_KEY) {
                    val result = NativeLib.replaceStoppedKeys(directory)
                    check(result >= 0) { AppLanguage.text(R.string.startup_key_write_failed) }
                    pending = pending.copy(directory = directory, issue = StartupIssue.KEY_SAVED)
                    _startupFailure.value = pending
                    LogManager.addLog("INFO", "Startup", "Confirmed device key replaced directory=$directory status=$result")
                    val publicKey = withContext(Dispatchers.IO) { provider.getString("node.public_key") }
                    if (publicKey?.length == 64) onEvent(11, publicKey.toByteArray(Charsets.US_ASCII))
                    check(result == 0) { AppLanguage.text(R.string.startup_key_sync_failed) }
                }
                if (!withContext(Dispatchers.IO) { provider.getServiceEnabled() }) {
                    LogManager.addLog("INFO", "Startup", "Recovery finished with service disabled")
                    _startupFailure.value = null
                } else {
                    checkNotNull(ContextCompat.startForegroundService(app, Intent(app, HeadlessService::class.java)))
                    LogManager.addLog("INFO", "Startup", "Recovery requested service start directory=$directory")
                }
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                _startupFailure.value = pending.copy(recreating = false, recoveryError = e.message ?: e.javaClass.simpleName)
                LogManager.addLog("ERROR", "Startup", "Recovery failed stage=${pending.issue}: $e")
            }
        }
    }

    override fun onEvent(type: Int, data: ByteArray?) {
        val sound = if (type == 1 || type == 32) try { messageSoundTarget(type, data) }
            catch (e: Exception) { LogManager.addLog("ERROR", "Events", "Sound event decode failed type=$type: $e"); null } else null
        if (!mainHandler.post {
            fun deliver(owner: String, work: () -> Unit) {
                try { work() } catch (e: Exception) { LogManager.addLog("ERROR", "Events", "Event handler failed owner=$owner type=$type: $e") }
            }
            if (type == 12) {
                _startupFailure.value = null
                deliver("activity") { com.utun.chat.data.CoreActivity.onCoreStarted() }
                deliver("BLE") { com.utun.chat.data.BleRadio.applySettings() }
                deliver("phone") { com.utun.chat.data.PhoneStateMonitor.refresh() }
                deliver("service") { com.utun.chat.headless.HeadlessService.onCoreStarted() }
            }
            if (type == 75) deliver("startup failure") {
                val failure = StartupFailure.decode(data)
                if (failure == null) LogManager.addLog("ERROR", "Startup", "Invalid failure payload bytes=${data?.size}; recreation disabled")
                val reported = failure ?: StartupFailure(AppLanguage.text(R.string.startup_core_failed), "", StartupIssue.ERROR)
                _startupFailure.value = reported
                LogManager.addLog("ERROR", "Startup", reported.message)
                com.utun.chat.headless.HeadlessService.onCoreFailed(reported.message)
            }
            if (type == 13 && !NativeLib.isCoreStopping())
                deliver("service stop") { com.utun.chat.headless.HeadlessService.onCoreStopped() }
            if (type == 12 || type == 13 || type == 65) deliver("radio call") { com.utun.chat.data.RadioCallController.onEvent(type, data) }
            if (type == 12 || type == 13 || type == 70 || type == 71) deliver("devgroup") { com.utun.chat.data.DevgroupRepository.onEvent(type, data) }
            if (type == 13 || type == 77 || type == 78) deliver("clipboard") { com.utun.chat.data.ClipboardExchange.onEvent(type, data) }
            if (type == 13 || type in 61..64) deliver("discovery") { com.utun.chat.data.LocalDiscovery.onEvent(type, data) }
            if (type == 13 || type in 49..50) deliver("member files") { ChatApplication.instance.memberFiles.onEvent(type, data) }
            if (type in listOf(4, 8, 9, 12, 13, 23, 24, 25, 31, 70, AvatarProtocol.GROUP_SNAPSHOT_EVENT) || type in 49..52) {
                deliver("members") { ChatApplication.instance.members.onEvent(type, data) }
                deliver("avatars") { ChatApplication.instance.avatars.onEvent(type, data) }
            }
            if (sound != null) deliver("sound") { NotificationSound.play(sound) }
            if (type == 12 || type == 13 || type in 34..40 || type == 42 || type == 44 || type == 66)
                deliver("call") { CallController.onEvent(type, data) }
            // Emit ждёт свободного места; неподписанная ViewModel не получает старые команды/ACK при создании.
            if (type != 77 && type != 78) eventScope.launch { _events.emit(type to data) }
        }) LogManager.addLog("ERROR", "Events", "Main event delivery rejected type=$type bytes=${data?.size}")
    }

    // Звук живёт в приложении: работает без Activity и не повторяется при replay событий в UI.
    private fun messageSoundTarget(type: Int, data: ByteArray?): String? {
        if (type == 1) {
            val event = ChatMessageEvent.decode(data) ?: return null
            if (event.initial) return null
            val self = NativeLib.getMySigningKey() ?: return null
            return if (event.author.contentEquals(self)) null else event.channel
        }
        val length = data?.firstOrNull()?.toInt()?.and(0xFF) ?: 0
        if (data == null || length == 0 || data.size != 1 + length + 32) {
            LogManager.addLog("WARN", "Sound", "Invalid message event: type=$type length=$length size=${data?.size}")
            return null
        }
        val author = data.copyOfRange(1 + length, data.size)
        val self = NativeLib.getMySigningKey() ?: return null
        if (author.contentEquals(self)) return null
        val id = String(data, 1, length, Charsets.UTF_8)
        val soundId = "dm:$id"
        LogManager.addLog("DEBUG", "Sound", "Incoming message sound dispatched: type=$type target=$soundId")
        return soundId
    }
}

class ChatApplication : Application() {
    private val applicationScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    lateinit var logManager: LogManager
    lateinit var configProvider: ConfigProvider
    lateinit var members: MemberRepository
    lateinit var avatars: AvatarRepository
    lateinit var memberFiles: MemberFilesRepository

    override fun onCreate() {
        super.onCreate()
        instance = this
        logManager = LogManager
        configProvider = ConfigProvider(this)
        applicationScope.launch {
            configProvider.streamLowLatency.collect { low ->
                com.utun.chat.data.StreamPlayerEngine.lowLatency = low
                LogManager.addLog("INFO", "stream", "Playback policy lowLatency=$low shared=true")
            }
        }
        members = MemberRepository()
        avatars = AvatarRepository(this, members)
        memberFiles = MemberFilesRepository(this)

        // До первого подтверждённого снимка не включаем звук у потенциально отключённого канала.
        NotificationSound.enabled = false
        applicationScope.launch {
            configProvider.changes.observe("notification sound", configProvider.notificationSoundSettings).collect { (enabled, muted) ->
                NotificationSound.mutedChannels = muted
                NotificationSound.enabled = enabled
                LogManager.addLog("DEBUG", "Sound", "Preferences applied enabled=$enabled muted=${muted.size}")
            }
        }

        /* One-time event callback registration — survives ViewModel recreation */
        NativeLib.setEventCallback(AppEventHandler)
        try { com.utun.chat.data.BleRadio.init() }
        catch (e: Exception) { LogManager.addLog("ERROR", "discovery", "BLE initialization failed: $e") }
        applicationScope.launch {
            configProvider.changes.observe("Bluetooth", configProvider.bluetoothSettings).collect { flags ->
                try { com.utun.chat.data.BleRadio.applySettings(flags) }
                catch (e: Exception) { LogManager.addLog("ERROR", "discovery", "BLE preference application failed flags=$flags: $e") }
            }
        }

        /* Incoming-call system effects (wake + ringtone + notification) — app-scoped */
        IncomingCallManager.init()

        /* Telecom self-managed call integration (system audio routing for calls) */
        TelecomCallManager.init()
        CallController.init()
        com.utun.chat.data.PhoneStateMonitor.init()
        // Только тестовые codec кадры: разрешения камеры, Activity, Surface и сеть не нужны.
        try { Thread({
            val started = System.nanoTime()
            LogManager.addLog("INFO", "call", "video codec background preparation begin; camera and network unused")
            try {
                val decoders = CallVideoCodec.probeDecoders()
                val encoders = CallVideoCodec.probe()
                LogManager.addLog("INFO", "call", "video codec background preparation complete " +
                    "elapsedMs=${(System.nanoTime() - started) / 1_000_000} decodeCodecs=${decoders.size} encodeCodecs=${encoders.size}")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "call", "video codec background preparation failed: ${e.message}")
            }
        }, "call-video-probe").start() } catch (e: Exception) {
            LogManager.addLog("ERROR", "call", "Cannot start optional video codec preparation: $e")
        }
    }

    companion object {
        lateinit var instance: ChatApplication
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        AppLanguage.applied()
    }
}
