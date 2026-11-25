package com.utun.chat.data

import com.utun.chat.R
import android.content.BroadcastReceiver
import android.content.Intent
import android.content.IntentFilter
import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import com.utun.chat.ChatApplication
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.concurrent.Executors
import java.util.concurrent.Future

/**
 * Полудуплексный аудио-движок рации с независимыми владельцами ручного PTT и автоматикой.
 *
 * Потоки:
 *  - io-поток: сериализованные setup/teardown AudioTrack и подписки общего микрофона;
 *  - capture-поток: очередь MicrophoneCapture (960 PCM frames/канал и PTT) → NativeLib.radioAudioFeed;
 *  - play-поток:    NativeLib.radioAudioPull (C: per-source jitter → декод → микшер) → AudioTrack.write
 *
 * Воспроизведение тянет микшированный PCM всех говорящих из единого голосового стека
 * в C (radio_audio): per-source jitter + time-stretch + микшер.
 * Ручные talkBegin/talkEnd/cancelTalk вызываются только на main. Последний release даёт хвост 250 мс;
 * повторное нажатие снимает таймер, cancel завершает запрос без хвоста. Отказ постановки таймера
 * логируется как ERROR и немедленно завершает ручной запрос; автоматика/измеритель учитываются отдельно.
 * talkingFlow показывает запрос/наблюдаемый TX, transmittingGroup — фактический native TX своего канала.
 * Подписка/native capture и workers сохраняются до запуска API/Thread: частичный отказ получает rollback.
 * Ошибка capture останавливает его подписку/encoder, сохраняя приём; ошибка playback освобождает всю свою сессию.
 * Очистка идёт на io независимо от доставки main; identity/поколение защищают заменившую сессию.
 */
class RadioAudioEngine(private val nowMs: () -> Long = SystemClock::elapsedRealtime) {

    companion object {
        const val SAMPLE_RATE = 48000
        const val FRAME_MS = 20
        const val FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000   // 960 (на канал)

        /** Воспроизведение всегда стерео (моно-пиры апмиксуются Opus-декодером). */
        const val PLAY_CHANNELS = 2

        const val RELEASE_HOLD_MS = 250L
        const val VAD_BACKGROUND_MS = 15 * 60 * 1000L
        const val MICROPHONE_READY_MS = 3 * 60 * 1000L

        /** Процессный синглтон: доступен из Activity и оверлей-сервиса. */
        @Volatile private var inst: RadioAudioEngine? = null
        fun instance(): RadioAudioEngine = inst ?: synchronized(this) {
            inst ?: RadioAudioEngine().also { inst = it }
        }

    }

    @Volatile private var captureSubscription: MicrophoneCapture.Subscription? = null
    private var captureThread: Thread? = null
    @Volatile private var audioTrack: AudioTrack? = null
    private var playThread: Thread? = null
    private var outputLease: AutoCloseable? = null
    private var capturePttRequested = false
    private var nativeCaptureOwned = false // Только io: capture start мог иметь эффект; сбрасывается после stop API.

    @Volatile private var active = false
    @Volatile private var groupId = ""
    private val mutableGroup = MutableStateFlow<String?>(null)
    val activeGroup: StateFlow<String?> = mutableGroup
    @Volatile private var talking = false
    @Volatile private var captureRunning = false
    @Volatile private var playbackRunning = false
    @Volatile private var requestedTalk = false
    private val pttHolds = RadioPttHolds()
    @Volatile private var talkRequestNs = 0L
    @Volatile private var generation = 0L
    private var audioGroup = ""
    private val vadWindow = RadioVadWindow(VAD_BACKGROUND_MS)
    @Volatile private var screenReceiver: BroadcastReceiver? = null
    @Volatile private var screenInteractive = false
    private var autoSettings = RadioAutoSettings()
    private var settingsReady = false
    private var settingsRequest = 0L
    private var settingsTimeout: Runnable? = null // Только io: текущая отменяемая проверка ACK.
    private var captureRequestedMode = RadioAutoSettings.OFF
    @Volatile private var autoMode = RadioAutoSettings.OFF
    @Volatile private var levelMonitoring = false
    @Volatile private var keepMicrophoneReady = false
    @Volatile private var captureHoldUntilMs = 0L
    private var captureHoldTimeout: Runnable? = null // Только io; callback проверяет владельца и поколение.
    private val _captureWanted = MutableStateFlow(false)
    val captureWanted: StateFlow<Boolean> = _captureWanted // Основание microphone foreground-service, независимо от окна PTT.

    private val _talkingFlow = MutableStateFlow(false)
    val talkingFlow: StateFlow<Boolean> = _talkingFlow
    /** Реальный PCM уже поступает из захвата; сгенерированная тишина ожидания не подтверждает готовность. */
    private val _captureReady = MutableStateFlow(false)
    val captureReady: StateFlow<Boolean> = _captureReady
    /** Фактический TX (PTT/автоматика), привязанный к каналу. Capture публикует, io сбрасывает после join. */
    private val _transmittingGroup = MutableStateFlow<String?>(null)
    val transmittingGroup: StateFlow<String?> = _transmittingGroup

    /* Сериализованный поток для setup/teardown аудио (не блокирует main). */
    private val io = Executors.newSingleThreadExecutor { r ->
        Thread(r, "radio-audio-io").apply { isDaemon = true }
    }

    /* Таймер окна автоматики; управление ресурсами всегда на io-потоке. */
    private val mainHandler = Handler(Looper.getMainLooper())
    private val releaseTalk = Runnable { finishTalk() }
    private val vadTimeout = Runnable {
        io.execute {
            if (active && vadWindow.deadlineMs > 0L && !vadWindow.isAllowed(SystemClock.elapsedRealtime())) {
                LogManager.addLog("INFO", "radio", "Auto background window expired (15 minutes) group=$audioGroup")
                updateCapture()
            }
        }
    }
    private val _autoActiveFlow = MutableStateFlow(false)
    val autoActiveFlow: StateFlow<Boolean> = _autoActiveFlow

    fun isActive(): Boolean = active

    fun isTalking(): Boolean = _talkingFlow.value

    /** Текущий маршрут выхода рации (SPEAKER/HEADSET). */
    fun route(): AudioRoute = if (MediaAudioController.state.value.active == MediaAudioProfile.PHONE) AudioRoute.SPEAKER else AudioRoute.HEADSET

    /** На main запросить прослушивание ненулевой группы. Setup/cleanup выполняет io; onReady вызывается на main.
     * Ошибка очистки/запуска даёт false. Новый start/stop инвалидирует прежний результат, также возвращая false. */
    private var invitationOwner = 0L // ID незавершённого приглашения, только для вновь созданного аудио.

    fun start(group: String, onReady: ((Boolean) -> Unit)? = null) = startOwned(group, 0, onReady)

    /** После SELECT ядра; уже работающее аудио сохраняет независимое владение. */
    fun startForInvitation(group: String, id: Long, onReady: (Boolean) -> Unit) = startOwned(group, id, onReady)

    /** На main отменить только аудио, созданное данной попыткой. false — оно уже принадлежит другому запуску. */
    fun cancelInvitation(id: Long): Boolean {
        checkMain("cancelInvitation")
        if (invitationOwner != id || id == 0L) return false
        stopForCall()
        LogManager.addLog("INFO", "radio", "Cancelled invitation audio id=$id")
        return true
    }

    private fun startOwned(group: String, invitation: Long, onReady: ((Boolean) -> Unit)?) {
        checkMain("start")
        if (NativeLib.isCoreStopping()) {
            LogManager.addLog("WARN", "radio", "start rejected during core cleanup group=$group")
            onReady?.invoke(false); return
        }
        if (group.isEmpty()) {
            LogManager.addLog("ERROR", "RadioAudio", "start rejected: zero group ID")
            onReady?.invoke(false)
            return
        }
        if (CallController.blocksMedia()) {
            LogManager.addLog("WARN", "radio", "start blocked by call group=$group")
            onReady?.invoke(false)
            return
        }
        if (active && groupId == group) {
            if (invitation == 0L) invitationOwner = 0 // Ручное действие сохраняет аудио независимо от приглашения.
            val version = generation
            io.execute { postReady(group, version, active && generation == version && audioTrack != null, onReady) }
            return
        }
        mainHandler.removeCallbacks(releaseTalk)
        val version = ++generation
        invitationOwner = invitation
        active = true
        groupId = group
        mutableGroup.value = group
        requestedTalk = false
        pttHolds.cancel()
        _talkingFlow.value = false
        io.execute {
            var ready = false
            var operation = "cleanup"
            try {
                if (active && generation == version) {
                    releaseLocked()
                    if (active && generation == version) {
                        audioGroup = group
                        vadWindow.restart(SystemClock.elapsedRealtime())
                        operation = "setup"
                        setupAudio(version)
                        ready = active && generation == version && audioTrack != null
                    }
                }
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "RadioAudio", "audio setup failed operation=$operation requestedGroup=$group " +
                    "audioGroup=$audioGroup generation=$version: $e")
                if (generation == version) active = false
                if (operation == "setup") try { releaseLocked() } catch (cleanup: Exception) {
                    LogManager.addLog("ERROR", "RadioAudio", "setup rollback failed group=$group generation=$version; retained for retry: $cleanup")
                }
            }
            postReady(group, version, ready, onReady)
        }
    }

    /** Доставить результат своего поколения на main; ошибка пользовательского callback логируется и передаётся caller. */
    private fun postReady(group: String, version: Long, ready: Boolean, onReady: ((Boolean) -> Unit)?) {
        if (mainHandler.post {
            val result = ready && active && generation == version && audioTrack != null
            try {
                onReady?.invoke(result)
                LogManager.addLog("DEBUG", "RadioAudio", "readiness group=$group generation=$version ready=$result")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "RadioAudio", "readiness callback failed group=$group generation=$version: $e")
                throw e
            } finally {
                if (!result && generation == version) {
                    active = false; mutableGroup.value = null
                    requestedTalk = false; pttHolds.cancel(); _talkingFlow.value = false
                }
            }
        }) return
        LogManager.addLog("ERROR", "RadioAudio", "readiness callback rejected group=$group generation=$version; stopping owned audio")
        if (generation != version) return
        active = false; mutableGroup.value = null; requestedTalk = false; _talkingFlow.value = false
        try { releaseLocked() } catch (e: Exception) {
            LogManager.addLog("ERROR", "RadioAudio", "undelivered setup cleanup failed group=$group; retained for retry: $e")
        }
    }

    /** На main снять логическое участие; Future завершается после join/очистки на io либо возвращает ошибку очистки.
     * Повторный stop повторяет незавершённые операции. Ждать Future нужно вне main. */
    fun stop(): Future<*> {
        checkMain("stop")
        invitationOwner = 0
        mainHandler.removeCallbacks(releaseTalk)
        active = false
        mutableGroup.value = null
        requestedTalk = false
        pttHolds.cancel()
        _talkingFlow.value = false
        ++generation
        return io.submit {
            try {
                releaseLocked()
                LogManager.addLog("INFO", "radio", "audio resources released group=$audioGroup")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "radio", "audio release failed group=$audioGroup: $e")
                throw e
            }
        }
    }

    /** Выключить участие в канале и вернуть завершение фактического освобождения аудиотракта. */
    fun stopForCall(): Future<*> {
        mutableGroup.value?.let { group ->
            NativeLib.radioSetActive(group, false)
            LogManager.addLog("INFO", "radio", "disabled for call group=$group")
        }
        val stopped = stop()
        RadioOverlayService.sync(ChatApplication.instance)
        return stopped
    }

    /** Восстановить режим прослушивания; старые удержания PTT уже сброшены в stop(). */
    fun restoreAfterCall(group: String) {
        if (active || CallController.blocksMedia()) {
            LogManager.addLog("WARN", "radio", "call restore skipped: audio busy group=$group active=$active")
            return
        }
        val channels = ChatRepository().getChannels()
        if (channels == null) { LogManager.addLog("ERROR", "radio", "call restore skipped: channel snapshot unavailable group=$group"); return }
        val exists = channels.any { it.id == group }
        if (!exists) { LogManager.addLog("WARN", "radio", "call restore skipped: channel removed group=$group"); return }
        NativeLib.radioSetActive(group, true)
        start(group) { ready ->
            val superseded = mutableGroup.value != group || CallController.blocksMedia()
            LogManager.addLog(if (ready || superseded) "INFO" else "ERROR", "radio",
                "restored after call group=$group ready=$ready superseded=$superseded")
            if (!ready && mutableGroup.value == group && !isActive()) {
                NativeLib.radioSetActive(group, false)
                stop()
            }
            RadioOverlayService.sync(ChatApplication.instance)
        }
    }

    /** Смена настроек сохраняет окно автоматики; явное включение открывает новое.
     * При отказе приём остаётся включённым, capture ждёт нового reconfigure/ACK; прежний ACK его не разрешает. */
    fun reconfigure(restartAuto: Boolean = false) {
        val version = generation
        io.execute {
            if (!active || generation != version || audioTrack == null) return@execute
            var operation = "capture stop"
            try {
                stopCapture()
                operation = "settings"
                loadCaptureSettings()
                if (restartAuto) vadWindow.restart(SystemClock.elapsedRealtime())
                operation = "capture update"
                updateCapture()
            } catch (e: Exception) {
                settingsTimeout?.let { mainHandler.removeCallbacks(it) }; settingsTimeout = null
                settingsReady = false; ++settingsRequest
                LogManager.addLog("ERROR", "RadioAudio", "radio reconfigure failed operation=$operation group=$audioGroup " +
                    "generation=$version; capture disabled until retry: $e")
                if (operation == "capture update") try { stopCapture() } catch (cleanup: Exception) {
                    LogManager.addLog("ERROR", "RadioAudio", "reconfigure rollback failed group=$audioGroup; retained for retry: $cleanup")
                }
            }
        }
    }

    /** Видимое приложение сбрасывает фоновое окно автоматики и отключает отсчёт. */
    fun setForeground(foreground: Boolean) {
        val nowMs = SystemClock.elapsedRealtime()
        io.execute {
            vadWindow.setForeground(foreground, nowMs)
            LogManager.addLog("DEBUG", "radio", "Auto application foreground=$foreground group=$audioGroup " +
                "backgroundLeftMs=${vadWindow.timeLeftMs(nowMs)}")
            if (active) updateCapture()
        }
    }

    /* Приём создаётся независимо от микрофона (только io-поток). */
    private fun setupAudio(version: Long) {
        loadCaptureSettings()
        val group = audioGroup
        check(NativeLib.radioAudioStart(group, 0)) { "native radioAudioStart failed group=$group" }

        // ── AudioTrack (воспроизведение, всегда стерео) ──
        val trackMin = AudioTrack.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        check(trackMin > 0) { "AudioTrack minimum buffer invalid group=$group value=$trackMin" }
        val trackBuf = (trackMin * 2).coerceAtLeast(FRAME_SAMPLES * PLAY_CHANNELS * 4)
        val track = AudioTrack(
            AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build(),
            AudioFormat.Builder().setSampleRate(SAMPLE_RATE)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build(),
            trackBuf, AudioTrack.MODE_STREAM, AudioManager.AUDIO_SESSION_ID_GENERATE)

        audioTrack = track

        outputLease = MediaAudioController.attachOutput(track)
        track.play()
        playbackRunning = true
        val worker = Thread({ playLoop(track, group, version) }, "radio-playback")
        playThread = worker
        worker.priority = Thread.MAX_PRIORITY
        worker.start()

        registerScreenReceiver(version)
        restartCaptureHold(nowMs(), "radio_enabled")
        updateCapture()
        LogManager.addLog("INFO", "RadioAudio", "listening group=$group trackBuf=$trackBuf")
    }

    /* Настройки загружаются при включении/перенастройке, без DataStore в пути PTT. */
    private fun loadCaptureSettings() {
        settingsTimeout?.let { mainHandler.removeCallbacks(it) }; settingsTimeout = null
        settingsReady = false
        val request = ++settingsRequest
        val provider = ChatApplication.instance.configProvider
        keepMicrophoneReady = provider.getRadioKeepMicrophoneReady()
        if (!keepMicrophoneReady) captureHoldUntilMs = 0
        MediaAudioController.refresh()
        autoSettings = provider.getRadioAutoSettings()
        if (!autoSettings.valid() || !NativeLib.radioConfigure(request, autoSettings)) {
            LogManager.addLog("ERROR", "radio", "Cannot apply automatic transmission settings request=$request settings=$autoSettings")
        } else {
            val timeout = object : Runnable {
                override fun run() {
                    io.execute {
                        if (!active || request != settingsRequest || settingsTimeout !== this) return@execute
                        settingsTimeout = null
                        if (!settingsReady) LogManager.addLog("ERROR", "radio",
                            "Automatic transmission settings acknowledgement timeout request=$request group=$audioGroup")
                    }
                }
            }
            settingsTimeout = timeout
            if (!mainHandler.postDelayed(timeout, 5000)) {
                settingsTimeout = null
                LogManager.addLog("ERROR", "radio", "settings timeout rejected request=$request group=$audioGroup; waiting for ACK without watchdog")
            }
        }
    }

    internal fun onSettingsApplied(request: Long, success: Boolean) {
        io.execute {
            if (!active || request != settingsRequest) return@execute
            settingsTimeout?.let { mainHandler.removeCallbacks(it) }; settingsTimeout = null
            settingsReady = success
            LogManager.addLog(if (success) "INFO" else "ERROR", "radio", "Automatic settings applied request=$request success=$success")
            if (success) updateCapture()
        }
    }

    /** Только видимая вкладка настроек владеет дополнительным захватом для измерителя. */
    fun setLevelMonitoring(enabled: Boolean) {
        levelMonitoring = enabled
        io.execute {
            LogManager.addLog("DEBUG", "radio", "Level monitoring=$levelMonitoring active=$active group=$audioGroup")
            if (active) updateCapture()
        }
    }

    /** Применить Android-настройку без переоткрытия микрофона и сброса DSP. */
    fun refreshMicrophonePolicy() {
        io.execute {
            val enabled = try { ChatApplication.instance.configProvider.getRadioKeepMicrophoneReady() } catch (e: Exception) {
                LogManager.addLog("ERROR", "radio", "Cannot read microphone standby setting group=$audioGroup: $e")
                return@execute
            }
            val changed = enabled != keepMicrophoneReady
            keepMicrophoneReady = enabled
            if (!enabled) captureHoldUntilMs = 0
            else if (changed && active) restartCaptureHold(nowMs(), "setting_enabled")
            LogManager.addLog("INFO", "radio", "Microphone standby enabled=$enabled active=$active group=$audioGroup")
            if (active) updateCapture()
        }
    }

    private fun restartCaptureHold(atMs: Long, reason: String) {
        if (!keepMicrophoneReady) return
        captureHoldUntilMs = maxOf(captureHoldUntilMs, atMs + MICROPHONE_READY_MS)
        LogManager.addLog("INFO", "radio", "Microphone standby renewed reason=$reason group=$audioGroup untilMs=$captureHoldUntilMs")
    }

    /** Только io. Передача и прогрев разделены: нужный захват также удерживает microphone foreground-service. */
    private fun publishCaptureWanted(wanted: Boolean) {
        if (_captureWanted.value == wanted) return
        _captureWanted.value = wanted
        if (inst === this && !mainHandler.post { if (inst === this) RadioOverlayService.sync(ChatApplication.instance) })
            LogManager.addLog("ERROR", "radio", "Microphone service sync rejected wanted=$wanted group=$audioGroup")
    }

    private fun cancelCaptureHoldTimeout() {
        captureHoldTimeout?.let { mainHandler.removeCallbacks(it) }
        captureHoldTimeout = null
    }

    /** Таймер ограничен поколением и своим callback: старый отбой не закрывает новый захват. */
    private fun scheduleCaptureHoldTimeout(version: Long, remainingMs: Long): Boolean {
        val timeout = object : Runnable {
            override fun run() {
                io.execute {
                    if (!active || generation != version || captureHoldTimeout !== this) return@execute
                    captureHoldTimeout = null
                    LogManager.addLog("INFO", "radio", "Microphone standby timer group=$audioGroup remainingMs=${captureHoldUntilMs - nowMs()}")
                    updateCapture()
                }
            }
        }
        captureHoldTimeout = timeout
        if (mainHandler.postDelayed(timeout, remainingMs)) return true
        captureHoldTimeout = null; captureHoldUntilMs = 0
        LogManager.addLog("ERROR", "radio", "Microphone standby timer rejected; idle capture disabled group=$audioGroup")
        return false
    }

    private fun screenOn(): Boolean =
        (ChatApplication.instance.getSystemService(Context.POWER_SERVICE) as PowerManager).isInteractive

    private fun registerScreenReceiver(version: Long) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                if (!active || generation != version || screenReceiver !== this) {
                    LogManager.addLog("DEBUG", "radio", "stale screen event ignored action=${intent.action} generation=$version")
                    return
                }
                if (intent.action != Intent.ACTION_SCREEN_ON && intent.action != Intent.ACTION_SCREEN_OFF) {
                    LogManager.addLog("WARN", "radio", "unknown screen event action=${intent.action} group=$audioGroup")
                    return
                }
                screenInteractive = intent.action == Intent.ACTION_SCREEN_ON
                if (!screenInteractive) cancelTalk()
                io.execute {
                    if (!active || generation != version || screenReceiver !== this) return@execute
                    if (intent.action == Intent.ACTION_SCREEN_OFF) {
                        vadWindow.close()
                        LogManager.addLog("INFO", "radio", "Auto screen off: window closed group=$audioGroup")
                    }
                    if (intent.action == Intent.ACTION_SCREEN_ON) {
                        vadWindow.restart(SystemClock.elapsedRealtime())
                        LogManager.addLog("INFO", "radio", "Auto screen on: new background window (15 minutes) group=$audioGroup")
                    }
                    updateCapture()
                }
            }
        }
        val filter = IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_ON)
            addAction(Intent.ACTION_SCREEN_OFF)
        }
        val ctx = ChatApplication.instance
        screenReceiver = receiver // Владение сохраняется также при исключении после регистрации на стороне ОС.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            ctx.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else ctx.registerReceiver(receiver, filter)
        screenInteractive = screenOn()
    }

    /** Оба детектора используют одно окно по экрану/фону; измеритель не продлевает это окно. */
    private fun updateCapture() {
        val version = generation
        try {
            mainHandler.removeCallbacks(vadTimeout)
            cancelCaptureHoldTimeout()
            if (!settingsReady) return
            if (!active || groupId != audioGroup) { publishCaptureWanted(false); stopCapture(); return }
            val now = SystemClock.elapsedRealtime()
            var hold = keepMicrophoneReady && screenInteractive && nowMs() < captureHoldUntilMs
            if (hold && !scheduleCaptureHoldTimeout(version, captureHoldUntilMs - nowMs())) hold = false
            vadWindow.setEnabled(autoSettings.enabled, now)
            var mode = radioCaptureMode(active, screenInteractive, vadWindow.isAllowed(now), autoSettings, requestedTalk, levelMonitoring, hold)
            LogManager.addLog("DEBUG", "radio", "Auto capture gate group=$audioGroup desired=$mode requested=${autoSettings.mode} " +
                "enabled=${autoSettings.enabled} screen=$screenInteractive foreground=${vadWindow.foreground} " +
                "backgroundLeftMs=${vadWindow.timeLeftMs(now)} manual=$requestedTalk monitor=$levelMonitoring capture=$captureRunning " +
                "standby=$hold standbyLeftMs=${(captureHoldUntilMs - nowMs()).coerceAtLeast(0)}")
            if (mode != null && mode != RadioAutoSettings.OFF && !vadWindow.foreground &&
                !mainHandler.postDelayed(vadTimeout, vadWindow.timeLeftMs(now).coerceAtLeast(1L))) {
                vadWindow.close()
                mode = radioCaptureMode(active, screenInteractive, false, autoSettings, requestedTalk, levelMonitoring, hold)
                LogManager.addLog("ERROR", "radio", "automatic timeout rejected group=$audioGroup; background window closed " +
                    "manual=$requestedTalk monitor=$levelMonitoring desired=$mode")
            }
            publishCaptureWanted(mode != null)
            if (mode == null) { stopCapture(); return }
            val detectorFailed = mode != RadioAutoSettings.OFF && autoMode == RadioAutoSettings.OFF
            if (captureSubscription != null && (captureRequestedMode != mode ||
                (detectorFailed && !requestedTalk && !levelMonitoring && !hold))) stopCapture()
            if (audioTrack == null) return
            if (captureSubscription == null && nativeCaptureOwned) stopCapture()
            if (!active || generation != version || groupId != audioGroup) return
            if (captureSubscription == null && !startCapture(mode, version)) {
                if (generation == version) {
                    requestedTalk = false
                    _talkingFlow.value = nativeCaptureOwned && _transmittingGroup.value != null
                }
                return
            }
            if (generation != version) return
            if (requestedTalk != capturePttRequested) {
                if (captureSubscription?.ptt(requestedTalk) != true) {
                    LogManager.addLog("ERROR", "radio", "PTT command rejected; stopping capture group=$audioGroup")
                    requestedTalk = false
                    stopCapture(); return
                }
                capturePttRequested = requestedTalk
            }
        } catch (e: Exception) {
            if (generation == version) {
                requestedTalk = false
                _talkingFlow.value = nativeCaptureOwned && _transmittingGroup.value != null
            }
            LogManager.addLog("ERROR", "RadioAudio", "capture update failed group=$audioGroup generation=$version current=$generation " +
                "nativeOwned=$nativeCaptureOwned; retained for retry: $e")
        }
    }

    /** Только io: сохранить подписку/native-владение до возможного эффекта API; при отказе выполнить rollback. */
    private fun startCapture(mode: Int, version: Long): Boolean {
        if (!MicrophonePermission.granted()) {
            LogManager.addLog("WARN", "radio", "Auto/PTT capture blocked: RECORD_AUDIO not granted group=$audioGroup mode=$mode")
            return false
        }
        val captureGroup = audioGroup
        var operation = "subscribe"
        try {
            val subscription = MediaAudioController.subscribe("radio")
            captureSubscription = subscription // Сохраняется до любых native-вызовов с возможным частичным эффектом.
            val channels = subscription.channels
            check(channels in 1..2) { "invalid capture channels=$channels" }
            operation = "native start"
            nativeCaptureOwned = true
            check(NativeLib.radioCaptureStart(captureGroup, channels, mode)) { "native capture start rejected" }
            operation = "mode"
            autoMode = NativeLib.radioAutoMode()
            check(autoMode == mode || autoMode == RadioAutoSettings.OFF) { "native capture mode mismatch requested=$mode actual=$autoMode" }
            captureRequestedMode = mode
            if (mode != RadioAutoSettings.OFF && autoMode == RadioAutoSettings.OFF) {
                check(requestedTalk || levelMonitoring || nowMs() < captureHoldUntilMs) { "Automatic transmission unavailable: capture cancelled" }
                LogManager.addLog("WARN", "radio", "Automatic transmission unavailable: keeping manual/monitor capture group=$captureGroup")
            }
            captureRunning = true
            _autoActiveFlow.value = autoMode != RadioAutoSettings.OFF
            // BEGIN должен попасть в uasync раньше первого кадра ручной передачи.
            operation = "begin"
            if (requestedTalk) {
                NativeLib.radioTalkBegin(captureGroup)
                talking = true
                _talkingFlow.value = true
            }
            capturePttRequested = requestedTalk
            operation = "worker start"
            val worker = Thread({
                try { captureLoop(subscription, captureGroup, channels) } finally {
                    if (captureRunning && active && generation == version && groupId == captureGroup) io.execute {
                        if (captureSubscription !== subscription || generation != version) return@execute
                        requestedTalk = false
                        try {
                            stopCapture()
                            cancelCaptureHoldTimeout(); captureHoldUntilMs = 0; publishCaptureWanted(false)
                            LogManager.addLog("WARN", "RadioAudio", "capture lost: resources released group=$captureGroup generation=$version")
                        } catch (e: Exception) {
                            LogManager.addLog("ERROR", "RadioAudio", "capture loss cleanup failed group=$captureGroup; retained for retry: $e")
                        }
                    }
                }
            }, "radio-capture")
            captureThread = worker
            worker.priority = Thread.MAX_PRIORITY
            worker.start()
            LogManager.addLog("INFO", "radio", "capture started group=$captureGroup ch=$channels mode=$autoMode shared=true")
            return true
        } catch (e: Exception) {
            if (generation == version) requestedTalk = false
            LogManager.addLog("ERROR", "RadioAudio", "capture startup failed operation=$operation group=$captureGroup " +
                "generation=$version mode=$mode: $e")
            try { stopCapture() } catch (cleanup: Exception) {
                LogManager.addLog("ERROR", "RadioAudio", "capture startup rollback failed group=$captureGroup; retained for retry: $cleanup")
            }
            return false
        }
    }

    /** Только io: закрыть подписку, дождаться worker и остановить encoder; незавершённый native stop повторяем. */
    private fun stopCapture() {
        captureRunning = false
        val subscription = captureSubscription
        var failure: Exception? = null
        var interrupted = false
        try {
            if (subscription != null) try {
                subscription.close()
                captureSubscription = null
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "RadioAudio", "capture subscription close failed group=$audioGroup: $e")
                failure = e
            }
            interrupted = joinWorker(captureThread, "capture")
            captureThread = null
            if (nativeCaptureOwned) try {
                NativeLib.radioCaptureStop()
                nativeCaptureOwned = false
                LogManager.addLog("INFO", "radio", "Auto/PTT capture released group=$audioGroup")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "RadioAudio", "native capture stop failed group=$audioGroup: $e")
                if (failure == null) failure = e
            }
            capturePttRequested = false
            talking = false
            _autoActiveFlow.value = false
            if (!nativeCaptureOwned) clearCaptureState()
            else _talkingFlow.value = requestedTalk || _transmittingGroup.value != null
            failure?.let { throw it }
        } finally {
            if (interrupted) Thread.currentThread().interrupt()
        }
    }

    /** Только после подтверждённого native capture/audio stop: снять наблюдаемый TX и режим детектора. */
    private fun clearCaptureState() {
        nativeCaptureOwned = false
        autoMode = RadioAutoSettings.OFF
        capturePttRequested = false
        talking = false
        publishCaptureReady(false, audioGroup)
        if (_transmittingGroup.value != null) {
            LogManager.addLog("DEBUG", "radio", "TX cleared after capture stop group=${_transmittingGroup.value}")
            _transmittingGroup.value = null
        }
        _talkingFlow.value = requestedTalk
        _autoActiveFlow.value = false
    }

    /** Дождаться своего worker до остановки DSP; interruption не разрешает освободить используемые им ресурсы. */
    private fun joinWorker(worker: Thread?, kind: String): Boolean {
        var interrupted = Thread.interrupted()
        if (worker != null) LogManager.addLog("DEBUG", "RadioAudio", "joining $kind worker=${worker.name} group=$audioGroup")
        while (worker?.isAlive == true) try { worker.join() } catch (e: InterruptedException) {
            interrupted = true
            LogManager.addLog("WARN", "RadioAudio", "$kind join interrupted group=$audioGroup; finishing cleanup")
        }
        if (worker != null) LogManager.addLog("DEBUG", "RadioAudio", "$kind worker joined group=$audioGroup")
        return interrupted
    }

    /** Только io: независимая очистка всех владельцев; первый отказ возвращается после остальных попыток. */
    private fun releaseLocked() {
        mainHandler.removeCallbacks(vadTimeout)
        cancelCaptureHoldTimeout(); captureHoldUntilMs = 0; publishCaptureWanted(false)
        settingsTimeout?.let { mainHandler.removeCallbacks(it) }; settingsTimeout = null
        settingsReady = false; ++settingsRequest
        vadWindow.setEnabled(false, SystemClock.elapsedRealtime())
        var failure: Exception? = null
        var interrupted = Thread.interrupted()
        try {
            screenReceiver?.let { receiver ->
                try {
                    ChatApplication.instance.unregisterReceiver(receiver)
                    screenReceiver = null
                    LogManager.addLog("DEBUG", "radio", "screen receiver removed group=$audioGroup")
                } catch (e: IllegalArgumentException) {
                    screenReceiver = null // ОС подтверждает отсутствие регистрации, в том числе после частичного отказа.
                    LogManager.addLog("WARN", "radio", "screen receiver already absent group=$audioGroup: $e")
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "radio", "screen receiver removal failed group=$audioGroup; retained for retry: $e")
                    failure = e
                }
            }
            try { stopCapture() } catch (e: Exception) { if (failure == null) failure = e }
            interrupted = Thread.interrupted() || interrupted
            playbackRunning = false
            val track = audioTrack
            if (playThread != null) try { track?.stop() } catch (e: Exception) {
                LogManager.addLog("WARN", "RadioAudio", "playback stop failed group=$audioGroup; release will follow: $e")
            }
            playThread?.interrupt()
            interrupted = joinWorker(playThread, "playback") || interrupted
            playThread = null
            try {
                outputLease?.close()
                outputLease = null
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "RadioAudio", "output detach failed group=$audioGroup; retained for retry: $e")
                if (e is InterruptedException) interrupted = true
                if (failure == null) failure = e
            }
            if (track != null && outputLease == null) try {
                track.release()
                audioTrack = null
                LogManager.addLog("DEBUG", "RadioAudio", "output released group=$audioGroup track=${System.identityHashCode(track)}")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "RadioAudio", "output release failed group=$audioGroup " +
                    "track=${System.identityHashCode(track)}; retained for retry: $e")
                if (failure == null) failure = e
            }
            try { NativeLib.radioAudioStop(); clearCaptureState() } catch (e: Exception) {
                LogManager.addLog("ERROR", "RadioAudio", "native audio stop failed group=$audioGroup: $e")
                if (failure == null) failure = e
            }
            failure?.let { throw it }
        } finally {
            if (interrupted) Thread.currentThread().interrupt()
        }
    }

    /** На main удержать ручной PTT от source; true — запрос принят, false — рация неактивна либо нет RECORD_AUDIO.
     * Запрос разрешения не сохраняет удержание: после выдачи нужно новое нажатие. */
    internal fun talkBegin(source: RadioPttSource = RadioPttSource.BUTTON): Boolean {
        checkMain("PTT")
        if (!active) {
            LogManager.addLog("WARN", "RadioAudio", "PTT begin ignored: inactive source=$source group=$groupId generation=$generation")
            return false
        }
        if (!screenInteractive) {
            LogManager.addLog("WARN", "RadioAudio", "PTT begin rejected: screen off source=$source group=$groupId")
            return false
        }
        if (!MicrophonePermission.granted()) {
            MicrophonePermission.request("PTT") {
                RadioOverlayService.sync(ChatApplication.instance)
                android.widget.Toast.makeText(ChatApplication.instance, AppLanguage.text(R.string.microphone_ptt_retry),
                    android.widget.Toast.LENGTH_LONG).show()
            }
            return false
        }
        mainHandler.removeCallbacks(releaseTalk)
        pttHolds.press(source)
        LogManager.addLog("DEBUG", "RadioAudio", "PTT hold source=$source transmitting=$requestedTalk group=$groupId")
        if (requestedTalk) return true
        talkRequestNs = SystemClock.elapsedRealtimeNanos()
        requestedTalk = true
        _talkingFlow.value = true
        val version = generation
        LogManager.addLog("DEBUG", "RadioAudio", "PTT requested group=$groupId")
        io.execute {
            if (active && generation == version) {
                LogManager.addLog("DEBUG", "RadioAudio", "PTT io queueMs=${(SystemClock.elapsedRealtimeNanos() - talkRequestNs) / 1_000_000}")
                updateCapture()
            }
        }
        return true
    }

    /** На main отпустить source. Последний владелец даёт хвост 250 мс; отказ таймера завершает запрос сразу. */
    internal fun talkEnd(source: RadioPttSource = RadioPttSource.BUTTON) {
        checkMain("PTT")
        if (!pttHolds.release(source)) return
        if (!active || !requestedTalk) return
        mainHandler.removeCallbacks(releaseTalk)
        if (!mainHandler.postDelayed(releaseTalk, RELEASE_HOLD_MS)) {
            LogManager.addLog("ERROR", "RadioAudio", "PTT release timer rejected source=$source " +
                "group=$groupId generation=$generation; ending manual request immediately")
            finishTalk()
            return
        }
        LogManager.addLog("DEBUG", "RadioAudio", "PTT release source=$source scheduled +${RELEASE_HOLD_MS}ms")
    }

    /** На main отменить source либо всех при null; последний владелец завершает запрос без хвоста, включая уже поставленный. */
    internal fun cancelTalk(source: RadioPttSource? = null) {
        checkMain("PTT")
        if (!pttHolds.cancel(source)) return
        mainHandler.removeCallbacks(releaseTalk)
        finishTalk()
    }

    private fun finishTalk() {
        if (!requestedTalk) return
        requestedTalk = false
        _talkingFlow.value = false
        val version = generation
        val endedAt = nowMs()
        LogManager.addLog("DEBUG", "RadioAudio", "PTT manual request cleared group=$groupId generation=$version")
        io.execute { if (active && generation == version) {
            if (screenInteractive) restartCaptureHold(endedAt, "ptt_end")
            updateCapture()
        } }
    }

    /** Защитить логическое состояние lifecycle/PTT и main-таймер до изменения запроса. */
    private fun checkMain(operation: String) = check(Looper.myLooper() == Looper.getMainLooper()) {
        val message = "$operation requires main thread: caller=${Thread.currentThread().name}"
        LogManager.addLog("ERROR", "RadioAudio", message)
        message
    }

    /** Capture worker публикует готовность; io сбрасывает её только после join. */
    private fun publishCaptureReady(ready: Boolean, group: String) {
        if (_captureReady.value == ready) return
        _captureReady.value = ready
        LogManager.addLog("DEBUG", "radio", "capture PCM ready=$ready group=$group")
    }

    /** Упорядочить PTT и PCM своей подписки; неверный кадр/отказ JNI завершает worker с этапом в логе. */
    private fun captureLoop(subscription: MicrophoneCapture.Subscription, group: String, ch: Int) {
        var expected = -1L
        var captureEpoch = -1L
        var discontinuity = false
        var frameN = 0L
        var reportedRequestNs = 0L
        var reportedSignalRequestNs = 0L
        var statsMs = SystemClock.elapsedRealtime()
        var statsFrames = 0
        var fedFrames = 0
        var skippedFrames = 0
        var minRmsDb = 0
        var maxRmsDb = -120
        var maxReadNs = 0L
        var maxFeedNs = 0L
        var previousFeedAllowed: Boolean? = null
        var operation = "poll"
        try {
            while (captureRunning && active && groupId == group) {
                operation = "poll"
                val readStartedNs = SystemClock.elapsedRealtimeNanos()
                val event = subscription.poll() ?: continue
                if (!captureRunning || !active || groupId != group) break
                if (event is MicrophoneCapture.Ptt) {
                    if (event.pressed && !screenInteractive) continue
                    operation = if (event.pressed) "control begin" else "control end"
                    if (event.pressed) NativeLib.radioTalkBegin(group) else NativeLib.radioTalkEnd(group)
                    talking = event.pressed
                    continue
                }
                val captured = event as MicrophoneCapture.Frame
                val ageNs = System.nanoTime() - captured.createdNs
                if (ageNs > 100_000_000) {
                    if (!discontinuity) LogManager.addLog("WARN", "radio", "stale capture group=$group ageMs=${ageNs / 1_000_000}")
                    publishCaptureReady(false, group)
                    discontinuity = true
                    continue
                }
                operation = "frame"
                check(captured.pcm.size == FRAME_SAMPLES * ch) {
                    "invalid capture frame samples=${captured.pcm.size} expected=${FRAME_SAMPLES * ch} sequence=${captured.sequence}"
                }
                if (discontinuity || (expected >= 0 && (captured.sequence != expected || captured.generation != captureEpoch))) {
                    operation = "discontinuity"
                    LogManager.addLog("INFO", "radio", "shared capture boundary group=$group epoch=$captureEpoch->${captured.generation}")
                    NativeLib.radioCaptureDiscontinuity(group)
                }
                captureEpoch = captured.generation
                discontinuity = false; expected = captured.sequence + FRAME_SAMPLES
                val buf = captured.pcm
                maxReadNs = maxOf(maxReadNs, SystemClock.elapsedRealtimeNanos() - readStartedNs)
                val inputDb = rmsDb(buf)
                minRmsDb = minOf(minRmsDb, inputDb)
                maxRmsDb = maxOf(maxRmsDb, inputDb)
                statsFrames++
                frameN++
                val autoAllowed = autoMode != RadioAutoSettings.OFF && screenInteractive && vadWindow.isAllowed(SystemClock.elapsedRealtime())
                val hold = keepMicrophoneReady && nowMs() < captureHoldUntilMs
                val feedAllowed = active && groupId == group && captureRunning && screenInteractive &&
                    (talking || autoAllowed || levelMonitoring || hold)
                if (previousFeedAllowed != feedAllowed) {
                    LogManager.addLog("DEBUG", "radio", "Auto PCM gate group=$group frame=$frameN allowed=$feedAllowed " +
                        "active=$active currentGroup=$groupId capture=$captureRunning manual=$requestedTalk talking=$talking " +
                        "nativeMode=$autoMode screen=$screenInteractive backgroundDeadlineMs=${vadWindow.deadlineMs}")
                    previousFeedAllowed = feedAllowed
                }
                if (feedAllowed) {
                    operation = "feed"
                    val requestNs = talkRequestNs
                    val feedStartedNs = SystemClock.elapsedRealtimeNanos()
                    check(NativeLib.radioAudioFeed(group, buf)) { "native PCM feed rejected" }
                    maxFeedNs = maxOf(maxFeedNs, SystemClock.elapsedRealtimeNanos() - feedStartedNs)
                    fedFrames++
                    if (requestedTalk && requestNs != reportedRequestNs) {
                        reportedRequestNs = requestNs
                        LogManager.addLog("INFO", "RadioAudio", "PTT first frame queued " +
                            "elapsedMs=${(SystemClock.elapsedRealtimeNanos() - requestNs) / 1_000_000} rmsDb=$inputDb epoch=$captureEpoch")
                    }
                    if (requestedTalk && requestNs != reportedSignalRequestNs && buf.any { it != 0.toShort() }) {
                        reportedSignalRequestNs = requestNs
                        LogManager.addLog("INFO", "RadioAudio", "PTT first nonzero PCM queued " +
                            "elapsedMs=${(SystemClock.elapsedRealtimeNanos() - requestNs) / 1_000_000} rmsDb=$inputDb epoch=$captureEpoch")
                    }
                    operation = "TX status"
                    if (captureRunning) _talkingFlow.value = requestedTalk || NativeLib.radioTransmitting()
                } else skippedFrames++
                if (captureRunning && active && groupId == group) publishCaptureReady(captured.hasCapture, group)
                operation = "TX status"
                val transmitting = if (captureRunning && active && groupId == group && NativeLib.radioTransmitting()) group else null
                if (_transmittingGroup.value != transmitting) {
                    if (_transmittingGroup.value == group && transmitting == null && autoMode != RadioAutoSettings.OFF &&
                        captureRunning && active && groupId == group && screenInteractive) {
                        val endedAt = nowMs()
                        io.execute { if (captureSubscription === subscription && active && groupId == group && screenInteractive) {
                            restartCaptureHold(endedAt, "automatic_tx_end")
                            updateCapture()
                        } }
                    }
                    _transmittingGroup.value = transmitting
                    LogManager.addLog("DEBUG", "radio", "TX state group=$group on=${transmitting != null} manual=$requestedTalk mode=$autoMode")
                }
                val nowMs = SystemClock.elapsedRealtime()
                if (nowMs - statsMs >= 10_000L) {
                    LogManager.addLog("DEBUG", "radio", "Auto capture summary group=$group spanMs=${nowMs - statsMs} " +
                        "frames=$statsFrames audioMs=${statsFrames * FRAME_MS} fed=$fedFrames skipped=$skippedFrames " +
                        "rmsDb=$minRmsDb..$maxRmsDb readMaxMs=${maxReadNs / 1_000_000.0} feedMaxMs=${maxFeedNs / 1_000_000.0} " +
                        "channels=$ch input=${MediaAudioController.state.value.inputName} " +
                        "route=${route()} mode=$autoMode screen=$screenInteractive manual=$requestedTalk tx=${_talkingFlow.value}")
                    statsMs = nowMs
                    statsFrames = 0; fedFrames = 0; skippedFrames = 0
                    minRmsDb = 0; maxRmsDb = -120; maxReadNs = 0L; maxFeedNs = 0L
                }
            }
        } catch (e: Exception) {
            if (e is InterruptedException && !captureRunning) {
                LogManager.addLog("DEBUG", "RadioAudio", "capture cancelled operation=$operation group=$group")
            } else {
                LogManager.addLog("ERROR", "RadioAudio", "capture failed operation=$operation group=$group channels=$ch " +
                    "frame=$frameN running=$captureRunning currentGroup=$groupId: $e cause=${e.cause}")
            }
        } finally {
            publishCaptureReady(false, group)
            LogManager.addLog("DEBUG", "RadioAudio", "capture loop done group=$group frames=$frameN")
        }
    }

    /** RMS непустого захваченного кадра в dBFS (0 = полная шкала; тишина = -120). */
    private fun rmsDb(buf: ShortArray): Int {
        var sum = 0L
        for (s in buf) sum += s.toLong() * s.toLong()
        val rms = Math.sqrt(sum.toDouble() / buf.size)
        return if (rms < 1.0) -120 else (20.0 * Math.log10(rms / 32768.0)).toInt()
    }

    /** Использовать только переданный track; ошибку своей сессии очистить на io до уведомления main. */
    private fun playLoop(track: AudioTrack, group: String, version: Long) {
        val frame = FRAME_SAMPLES * PLAY_CHANNELS
        val buf = ShortArray(frame)
        val frameNs = FRAME_MS * 1_000_000L   /* 20мс в наносекундах */
        var nextNs = System.nanoTime()
        var previousHead = 0L
        var playedFrames = 0L
        var operation = "head"
        var written = 0
        LogManager.addLog("DEBUG", "radio", "playback progress tracking group=$group guard=200ms")
        try {
            while (playbackRunning) {
                operation = "head"
                /* AudioTrack возвращает unsigned 32-bit позицию; разворачиваем wrap (~25 часов @48k). */
                val head = track.playbackHeadPosition.toLong() and 0xffffffffL
                playedFrames += (head - previousHead) and 0xffffffffL
                previousHead = head
                operation = "pull"
                val n = NativeLib.radioAudioPull(group, buf, playedFrames)
                check(n in 0..frame && n % PLAY_CHANNELS == 0) { "invalid native playback count=$n capacity=$frame" }
                if (n < frame) buf.fill(0, n, frame)
                written = 0
                operation = "write"
                while (written < frame && playbackRunning) {
                    val count = track.write(buf, written, frame - written)
                    if (!playbackRunning) return // stop мог разблокировать write с нулём/ошибкой; кадр больше не нужен.
                    check(count > 0 && count <= frame - written && count % PLAY_CHANNELS == 0) {
                        "invalid playback write rc=$count written=$written/$frame channels=$PLAY_CHANNELS"
                    }
                    written += count
                }

                /* абсолютный deadline: не даём Thread.sleep накопить систематический дрейф */
                nextNs += frameNs
                val waitNs = nextNs - System.nanoTime()
                if (waitNs > 0) {
                    operation = "sleep"
                    Thread.sleep(waitNs / 1_000_000L, (waitNs % 1_000_000L).toInt())
                } else {
                    nextNs = System.nanoTime()   /* отстали — сбрасываем, не копим долг */
                }
            }
        } catch (e: Exception) {
            if (e is InterruptedException && !playbackRunning) {
                LogManager.addLog("DEBUG", "RadioAudio", "playback cancelled operation=$operation group=$group generation=$version")
            } else {
                LogManager.addLog("ERROR", "RadioAudio", "playback failed operation=$operation group=$group generation=$version " +
                    "written=$written/$frame running=$playbackRunning: $e cause=${e.cause}")
                io.execute {
                    if (audioTrack !== track) {
                        LogManager.addLog("DEBUG", "RadioAudio", "obsolete playback failure ignored group=$group generation=$version")
                        return@execute
                    }
                    try { releaseLocked() } catch (cleanup: Exception) {
                        LogManager.addLog("ERROR", "RadioAudio", "playback failure cleanup failed group=$group; retained for retry: $cleanup")
                    }
                    postReady(group, version, false, null)
                }
            }
        } finally {
            LogManager.addLog("DEBUG", "RadioAudio", "play loop done group=$group generation=$version")
        }
    }

}
