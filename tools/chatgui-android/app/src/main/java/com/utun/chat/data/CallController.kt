package com.utun.chat.data

import android.content.Intent
import android.app.NotificationManager
import android.widget.Toast
import androidx.core.app.NotificationCompat
import com.utun.chat.R
import android.os.Looper
import android.os.SystemClock
import com.utun.chat.ChatApplication
import com.utun.chat.MainActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.Job
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.withContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Единственный владелец звонковой сессии в процессе приложения. Все операции — main thread.
 * Сервис владеет аудио и wake lock, ядро — сигналингом, ViewModel только читает StateFlow.
 * События поступают напрямую из AppEventHandler без replay старых звонков новым подписчикам.
 * Start/accept выполняют только сигналинг; media освобождаются после подтверждения выбора устройства.
 * Hangup/decline при отказе очереди
 * повторяются независимо от UI/audio cleanup. Core reset отменяет такие повторы до остановки worker.
 */
object CallController : TelecomCallManager.Listener {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val _state = MutableStateFlow<CallState?>(null)
    val state: StateFlow<CallState?> = _state
    @Volatile private var mediaPreparation: Job? = null
    private val mediaDispatcher = PhoneMediaDispatcher(scope)
    private var deferredTelecom = false
    private var incomingCall = false
    private var radioToRestore: String? = null
    private val pendingEnds = MutableStateFlow<Map<Long, Boolean>>(emptyMap()) // true=decline, false=hangup; main.
    private var endRetry: Job? = null

    /** Подготовка, разговор и неподтверждённый cleanup сервиса запрещают запуск чужого media.
     * Predicate читается также media workers; CallService публикует retained-владельца через volatile. */
    fun blocksMedia(): Boolean = pendingEnds.value.isNotEmpty() || CallService.cleanupPending() || TelecomCallManager.cleanupPending() ||
        mediaDispatcher.blocked || mediaPreparation?.isActive == true || _state.value?.let {
        it.phase == CallPhase.ACTIVE
    } == true

    fun hasMedia(): Boolean = RadioAudioEngine.instance().activeGroup.value != null ||
        StreamController.state.value?.ongoing == true || StreamPlayerEngine.viewing.value

    fun mediaWarning(incoming: Boolean): Int {
        val radio = RadioAudioEngine.instance().activeGroup.value != null
        val stream = StreamController.state.value?.ongoing == true || StreamPlayerEngine.viewing.value
        return when {
            radio && stream -> if (incoming) R.string.call_incoming_stop_media else R.string.call_outgoing_stop_media
            radio -> if (incoming) R.string.call_incoming_stop_radio else R.string.call_outgoing_stop_radio
            else -> if (incoming) R.string.call_incoming_stop_stream else R.string.call_outgoing_stop_stream
        }
    }

    private val _video = MutableStateFlow<CallVideoEngine?>(null)
    val video: StateFlow<CallVideoEngine?> = _video
    private val _videoActive = MutableStateFlow(false)
    val videoActive: StateFlow<Boolean> = _videoActive
    private var videoJob: Job? = null
    private var videoSpeakerPending = false
    fun setVideo(settings: CallVideoSettings, grant: android.content.Intent? = null) {
        checkMain()
        val owner = service ?: return
        if (_state.value?.phase != CallPhase.ACTIVE) return
        if (owner.setVideoCapture(settings.enabled, settings.screen, grant)) owner.video?.select(settings)
    }
    private val _muted = MutableStateFlow(false)
    val muted: StateFlow<Boolean> = _muted
    private val _route = MutableStateFlow(AudioRoute.SPEAKER)
    val route: StateFlow<AudioRoute> = _route
    private val _headset = MutableStateFlow(false)
    val headset: StateFlow<Boolean> = _headset
    private val _earpiece = MutableStateFlow(true)
    val earpiece: StateFlow<Boolean> = _earpiece
    private val proximity = ProximityManager(ChatApplication.instance)
    val proximityNear: StateFlow<Boolean?> = proximity.near
    private var proximityEnabled = false // До чтения сохранённой настройки датчик не запускаем.
    private var proximityThresholdCm = 5f

    private var service: CallService? = null

    fun refreshLanguage() { service?.refreshLanguage() }
    private var serviceRequested = false
    private var signallingStarted = false
    private var accepting = false
    private var acceptSent = false
    private var answerWhenVisible = 0L
    private var activityVisible = false
    private var coreReady = false
    private var incomingAt = 0L
    private var initialized = false

    fun init() {
        checkMain()
        if (initialized) return
        initialized = true
        TelecomCallManager.setListener(this)
        scope.launch {
            val provider = ChatApplication.instance.configProvider
            provider.changes.observe("call proximity settings", provider.proximitySettings).collect { (enabled, threshold) ->
                proximityEnabled = enabled; proximityThresholdCm = threshold
                LogManager.addLog("INFO", "Proximity", "Settings applied enabled=$enabled threshold=$threshold cm")
                syncProximity(); applyVideoSpeakerIfReady()
            }
        }
        scope.launch { proximity.near.collect { applyVideoSpeakerIfReady() } }
        scope.launch {
            while (true) {
                delay(500)
                val st = _state.value ?: continue
                val now = SystemClock.elapsedRealtime()
                if (st.phase == CallPhase.ACTIVE) {
                    _state.value = st.copy(durationSeconds = ((now - st.startedAtElapsedRealtime) / 1000).toInt())
                } else if (st.phase == CallPhase.INCOMING && now - incomingAt > 60_000) {
                    LogManager.addLog("WARN", "Call", "incoming timeout id=${st.callId}; declining")
                    end(st.callId)
                }
            }
        }
        LogManager.addLog("INFO", "Call", "controller initialized; session independent of UI")
    }

    fun setActivityVisible(visible: Boolean) {
        checkMain()
        activityVisible = visible
        CoreActivity.visible(visible)
        syncCoreActivity()
        if (visible && answerWhenVisible != 0L) {
            val id = answerWhenVisible
            answerWhenVisible = 0L
            if (_state.value?.callId == id && _state.value?.phase == CallPhase.INCOMING) accept()
        }
    }

    private fun syncCoreActivity() {
        CoreActivity.call(_state.value?.ongoing == true)
    }

    fun start(channelId: String, peerAddress: String?, peerName: String, devices: Boolean = false) {
        checkMain()
        if (RadioCallController.busy()) {
            LogManager.addLog("WARN", "Call", "start rejected: pending radio invitation")
            return
        }
        if (channelId.isEmpty() || (peerAddress == null && !channelId.startsWith("dm:")) ||
            (peerAddress != null && !MemberFilesProtocol.validAddress(peerAddress))) {
            LogManager.addLog("ERROR", "Call", "invalid outgoing call channel=$channelId peer=$peerAddress")
            return
        }
        if (_state.value?.ongoing == true || !coreReady || !NativeLib.isReady()) {
            LogManager.addLog("WARN", "Call", "start rejected: busy=${_state.value?.ongoing} core=$coreReady native=${NativeLib.isReady()}")
            return
        }
        if (mediaDispatcher.blocked || pendingEnds.value.isNotEmpty() || CallService.cleanupPending() || TelecomCallManager.cleanupPending()) {
            LogManager.addLog("WARN", "Call", "start rejected: previous signalling/service cleanup pending")
            return
        }
        if (!MicrophonePermission.granted()) {
            MicrophonePermission.request("Outgoing call") { start(channelId, peerAddress, peerName, devices) }
            return
        }
        var id = java.security.SecureRandom().nextLong()
        if (id == 0L) id = 1
        _state.value = CallState(id, channelId, peerAddress, peerName.ifEmpty { peerAddress ?: channelId },
            CallPhase.OUTGOING, devices = devices)
        deferredTelecom = StreamController.state.value?.ongoing == true || StreamPlayerEngine.viewing.value
        incomingCall = false
        syncCoreActivity()
        requestService(id)
    }

    fun accept() {
        checkMain()
        val st = _state.value ?: return
        if (st.phase != CallPhase.INCOMING || accepting) {
            LogManager.addLog("WARN", "Call", "accept ignored id=${st.callId} phase=${st.phase} accepting=$accepting")
            return
        }
        if (!MicrophonePermission.granted()) {
            MicrophonePermission.request("Answer call id=${st.callId}") {
                if (_state.value?.callId == st.callId && _state.value?.phase == CallPhase.INCOMING) accept()
                else LogManager.addLog("WARN", "Call", "Permission granted after incoming call ended id=${st.callId}")
            }
            return
        }
        accepting = true
        _state.value = st.copy(answering = true)
        IncomingCallManager.stopRinging()
        requestService(st.callId)
        startSignalling()
    }

    /** После ACCEPTED освободить чужие аудиотракты; отмена вызова не прерывает release. */
    private fun prepareMedia(id: Long, ready: suspend () -> Unit) {
        if (mediaDispatcher.blocked) return
        if (radioToRestore == null) radioToRestore = RadioAudioEngine.instance().activeGroup.value
        _state.value = _state.value?.copy(preparing = true)
        mediaDispatcher.enter(id, release = {
            LogManager.addLog("INFO", "Call", "media release begins id=$id restoreRadio=$radioToRestore")
            val radioStopped = RadioAudioEngine.instance().stopForCall()
            val voiceStopped = AudioRecorderManager.stopAll()
            StreamController.stop()
            StreamPlayerEngine.stopForCall()
            StreamController.stopForCall()
            withContext(Dispatchers.IO) { radioStopped.get(); voiceStopped.forEach { it.get() }; MediaAudioController.awaitIdle() }
        }, activate = {
            if (deferredTelecom && TelecomCallManager.isReady()) {
                val st = _state.value ?: error("Missing accepted call")
                val registered = if (incomingCall) TelecomCallManager.addIncomingCall(id, requireNotNull(st.peerAddress), st.peerName)
                    else TelecomCallManager.placeCall(id, st.peerAddress ?: st.channelId, st.peerName)
                check(registered) { "Telecom registration rejected" }
                while (TelecomCallManager.connectionFor(id) == null) {
                    if (_state.value?.callId != id || _state.value?.ongoing != true) return@enter
                    delay(25)
                }
            }
            ready()
        }, failed = { e ->
            LogManager.addLog("ERROR", "Call", "media release failed id=$id: $e")
            end(id, 7)
            try { Toast.makeText(ChatApplication.instance, R.string.call_media_stop_failed, Toast.LENGTH_LONG).show() }
            catch (notify: Exception) { LogManager.addLog("ERROR", "Call", "Cannot show media release failure id=$id: $notify") }
        })
        mediaPreparation = mediaDispatcher.job
    }

    private fun requestService(id: Long) {
        serviceRequested = true
        try {
            CallService.start(ChatApplication.instance, id)
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "Call", "service request failed id=$id: ${e.message}")
            end(id, 7)
        }
    }

    /** Вызывается сервисом после startForeground и приобретения wake lock, до запуска сигналинга/микрофона. */
    fun serviceReady(id: Long, owner: CallService): Boolean {
        checkMain()
        val st = _state.value
        if (st == null || st.callId != id || !st.ongoing || !serviceRequested) {
            LogManager.addLog("WARN", "Call", "discard stale service start id=$id current=${st?.callId}")
            return false
        }
        if (service != null) {
            if (service !== owner) LogManager.addLog("ERROR", "Call", "service owner conflict id=$id")
            return service === owner
        }
        service = owner
        LogManager.addLog("INFO", "Call", "service ready id=$id phase=${st.phase}")
        startSignalling()
        return true
    }

    /** Сервис резервируется видимым UI сразу, сигналинг ждёт фактического освобождения медиа. */
    private fun startSignalling() {
        val st = _state.value ?: return
        if (service == null || !st.ongoing || st.preparing || signallingStarted) return
        val id = st.callId
        if (st.phase == CallPhase.OUTGOING) {
            if (!NativeLib.callStart(st.channelId, st.peerAddress, id, st.devices)) {
                LogManager.addLog("ERROR", "Call", "start queue rejected id=$id; releasing reserved service")
                finish(id, 7); return
            }
            signallingStarted = true
            if (!deferredTelecom) TelecomCallManager.placeCall(id, st.peerAddress ?: st.channelId, st.peerName)
            CallRingback.startConnecting()
        } else if (st.phase == CallPhase.INCOMING && accepting) {
            IncomingCallManager.stopRinging()
            if (!NativeLib.callAccept(id)) {
                LogManager.addLog("ERROR", "Call", "accept queue rejected id=$id; declining and releasing service")
                end(id, 7); return
            }
            signallingStarted = true
            acceptSent = true
        }
    }

    fun wantsService(id: Long): Boolean = _state.value?.let { it.callId == id && it.ongoing && serviceRequested } == true

    fun serviceLost(id: Long, owner: CallService) {
        checkMain()
        if (service !== owner) return
        LogManager.addLog("ERROR", "Call", "service destroyed during call id=$id")
        end(id, 7)
    }

    fun audioFailed(id: Long, detail: String) {
        checkMain()
        if (_state.value?.callId != id || _state.value?.ongoing != true) return
        LogManager.addLog("ERROR", "Call", "audio failed id=$id: $detail")
        end(id, 7)
    }

    /** Завершение из UI, Telecom и уведомления; идентификатор защищает от действий старого звонка. */
    fun end(id: Long = _state.value?.callId ?: 0L, reason: Int = 0) {
        checkMain()
        val st = _state.value ?: return
        if (st.callId != id || !st.ongoing) {
            LogManager.addLog("WARN", "Call", "end ignored id=$id current=${st.callId} phase=${st.phase}")
            return
        }
        if (NativeLib.isReady()) {
            if ((st.phase == CallPhase.INCOMING || st.phase == CallPhase.PENDING_INCOMING) && !acceptSent) queueEnd(id, decline = true)
            else if (signallingStarted) queueEnd(id, decline = false)
        }
        finish(id, reason)
    }

    /** Ошибка постановки не удерживает FGS/audio, но сохраняет команду и запрещает новый media до queue ACK. */
    private fun queueEnd(id: Long, decline: Boolean) {
        if (sendEnd(id, decline)) { pendingEnds.value = pendingEnds.value - id; return }
        pendingEnds.value = pendingEnds.value + (id to decline)
        if (endRetry?.isActive == true) return
        endRetry = scope.launch {
            var pause = 250L
            while (pendingEnds.value.isNotEmpty()) {
                delay(pause)
                if (!coreReady || !NativeLib.isReady()) { pendingEnds.value = emptyMap(); return@launch }
                for ((call, reject) in pendingEnds.value) if (sendEnd(call, reject)) {
                    pendingEnds.value = pendingEnds.value - call
                    LogManager.addLog("INFO", "Call", "Deferred ${if (reject) "decline" else "hangup"} queued id=$call")
                }
                pause = (pause * 2).coerceAtMost(30_000)
            }
        }
    }
    private fun sendEnd(id: Long, decline: Boolean): Boolean = try {
        val queued = if (decline) NativeLib.callDecline(id) else NativeLib.callHangup(id)
        if (!queued) LogManager.addLog("ERROR", "Call", "${if (decline) "decline" else "hangup"} queue rejected id=$id; retained for retry")
        queued
    } catch (e: Exception) {
        LogManager.addLog("ERROR", "Call", "End command threw id=$id decline=$decline; retained for retry: $e")
        false
    }

    private fun finish(id: Long, reason: Int) {
        val st = _state.value ?: return
        if (st.callId != id || !st.ongoing) return
        val owner = service
        mediaDispatcher.cancel(id)
        service = null
        serviceRequested = false
        signallingStarted = false
        accepting = false
        acceptSent = false
        answerWhenVisible = 0L
        videoJob?.cancel(); videoJob = null; videoSpeakerPending = false; _videoActive.value = false
        owner?.shutdown()
        _video.value = null
        proximity.stop()
        TelecomCallManager.setDisconnected(id, reason)
        TelecomCallManager.removeConnection(id)
        CallRingback.stop()
        IncomingCallManager.stopRinging()
        _muted.value = false
        _route.value = AudioRoute.SPEAKER
        _headset.value = false
        _earpiece.value = true
        _state.value = st.copy(phase = CallPhase.ENDED, reason = reason, preparing = false, answering = false,
            connectionType = CallConnectionType.NONE, connectionNextHop = 0)
        syncCoreActivity()
        LogManager.addLog("INFO", "Call", "finished id=$id reason=$reason serviceCleanupComplete=${!CallService.cleanupPending()}")
        val released = mediaDispatcher.job
        scope.launch {
            var warned = false
            while (CallService.cleanupPending()) {
                CallService.pendingRelease?.join()
                if (CallService.cleanupPending()) {
                    if (!warned) LogManager.addLog("WARN", "Call", "service completion ended before resource cleanup id=$id; waiting for owner")
                    warned = true
                    delay(25)
                }
            }
            released?.join()
            TelecomCallManager.awaitReleased()
            pendingEnds.first { it.isEmpty() }
            mediaDispatcher.leave(id)
            if (!coreReady || !NativeLib.isReady() || blocksMedia()) return@launch
            val group = radioToRestore ?: return@launch
            radioToRestore = null
            RadioAudioEngine.instance().restoreAfterCall(group)
        }
        if (reason == 8 && st.phase != CallPhase.INCOMING && st.phase != CallPhase.PENDING_INCOMING) try {
            val app = ChatApplication.instance
            val text = AppLanguage.text(R.string.phone_busy_warning)
            if (activityVisible) Toast.makeText(app, text, Toast.LENGTH_LONG).show()
            else {
                requireNotNull(app.getSystemService(NotificationManager::class.java)) { "notification manager unavailable" }.notify(1003,
                    NotificationCompat.Builder(app, IncomingCallManager.CHANNEL_CALL)
                        .setSmallIcon(android.R.drawable.ic_dialog_info).setContentTitle(st.peerName)
                        .setContentText(text).setAutoCancel(true).build())
            }
        } catch (e: Exception) { LogManager.addLog("ERROR", "Call", "Busy outcome notification failed id=$id: $e") }
        scope.launch {
            delay(2500)
            if (_state.value?.callId == id && _state.value?.phase == CallPhase.ENDED) _state.value = null
        }
    }

    fun dismiss() {
        checkMain()
        if (_state.value?.ongoing == true) end()
        _state.value = null
    }

    /** Перед stop/restart снять сессию и запросить shutdown. NativeLib отдельно проверяет подтверждение
     * CallService и окончание mediaPreparation; retry не разрешает разрушить используемые native-ресурсы. */
    fun reset() {
        checkMain()
        radioToRestore = null
        coreReady = false
        CoreActivity.reset()
        end()
        endRetry?.cancel(); endRetry = null; pendingEnds.value = emptyMap()
        _state.value = null
        CoreActivity.call(false)
    }

    fun toggleMute() {
        checkMain()
        val audio = service?.audio ?: return
        if (!audio.isActive()) return
        audio.setMuted(!audio.isMuted())
        _muted.value = audio.isMuted()
    }

    fun setRoute(route: AudioRoute) {
        checkMain()
        if (_state.value?.phase != CallPhase.ACTIVE || service == null) {
            LogManager.addLog("WARN", "Call", "route ignored outside active call route=$route phase=${_state.value?.phase}")
            return
        }
        videoSpeakerPending = false // Явный выбор пользователя отменяет ожидающее автоматическое переключение.
        service?.audio?.router?.setRoute(route)
        syncProximity()
        scope.launch {
            try { ChatApplication.instance.configProvider.setCallAudioRoute(route) }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                LogManager.addLog("ERROR", "Call", "Cannot save preferred route=$route: $e")
                Toast.makeText(ChatApplication.instance, R.string.call_route_save_failed, Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun routeChanged(route: AudioRoute, headset: Boolean) {
        if (_route.value != route || _headset.value != headset) LogManager.addLog("INFO", "Call",
            "audio controls id=${_state.value?.callId} route=$route headset=$headset")
        _route.value = route
        _headset.value = headset
        if (route != AudioRoute.EARPIECE) videoSpeakerPending = false
        syncProximity()
        service?.audio?.syncAecForRoute(route)
    }

    override fun onConnectionAvailable(callId: Long, connection: UtunConnection) {
        checkMain()
        if (_state.value?.callId == callId && _state.value?.phase == CallPhase.ACTIVE)
            service?.audio?.bindTelecom(connection)
    }

    private fun syncProximity() {
        if (proximityEnabled && _state.value?.phase == CallPhase.ACTIVE && _route.value == AudioRoute.EARPIECE
            && (!_videoActive.value || videoSpeakerPending)) proximity.start(proximityThresholdCm) else proximity.stop()
    }

    private fun applyVideoSpeakerIfReady() {
        if (!videoSpeakerPending || !_videoActive.value || _route.value != AudioRoute.EARPIECE) return
        if (proximityEnabled && proximity.near.value != false) return
        val st = _state.value ?: return
        val audio = service?.audio ?: return
        if (st.phase != CallPhase.ACTIVE) return
        videoSpeakerPending = false
        // Сохранённый маршрут аудиозвонков не меняем; автоследование за гарнитурой сохраняется.
        audio.router.fallbackRoute = AudioRoute.SPEAKER
        audio.router.setRoute(AudioRoute.SPEAKER, automatic = true)
        syncProximity()
        LogManager.addLog("INFO", "call", "video speaker requested proximityEnabled=$proximityEnabled near=${proximity.near.value} id=${st.callId}")
    }

    /** Только живые события ядра, сериализованные AppEventHandler на main thread. */
    fun onEvent(type: Int, data: ByteArray?) {
        checkMain()
        if (type == 12) {
            coreReady = NativeLib.isReady()
            syncCoreActivity()
            LogManager.addLog("INFO", "Call", "core started ready=$coreReady")
            return
        }
        if (type == 13) { reset(); return }
        if (type !in 34..40 && type != 42 && type != 44 && type != 66) return
        if (!coreReady || !NativeLib.isReady()) {
            LogManager.addLog("WARN", "Call", "ignore event=$type while core stopped/restarting")
            return
        }
        if (type == 34) {
            if (data == null || data.size < 42) { badEvent(type, data); return }
            val len = data[0].toInt() and 0xFF
            if (len == 0 || data.size != 41 + len) { badEvent(type, data); return }
            val channel = String(data, 1, len)
            val buf = ByteBuffer.wrap(data, 1 + len, 40).order(ByteOrder.LITTLE_ENDIAN)
            val peer = MemberFilesProtocol.addressHex(ByteArray(32).also { buf.get(it) })
            val id = buf.long
            if (peer.isEmpty() || id == 0L) { badEvent(type, data); return }
            val current = _state.value
            if (current?.callId == id) return
            if (current?.ongoing == true || mediaDispatcher.blocked || CallService.cleanupPending() || TelecomCallManager.cleanupPending()) {
                LogManager.addLog("WARN", "Call", "busy; decline incoming id=$id current=${current?.callId} media=${mediaDispatcher.blocked}")
                queueEnd(id, decline = true)
                return
            }
            incomingAt = SystemClock.elapsedRealtime()
            // Однократное чтение на main завершается до reset; фоновый JNI-запрос не переживает ядро.
            val name = ChatRepository().getMembers(channel).firstOrNull { it.ed25519 == peer }?.name
                ?.takeIf { it.isNotEmpty() } ?: peer
            deferredTelecom = StreamController.state.value?.ongoing == true || StreamPlayerEngine.viewing.value
            incomingCall = true
            _state.value = CallState(id, channel, peer, name,
                if (TelecomCallManager.isReady() && !deferredTelecom) CallPhase.PENDING_INCOMING else CallPhase.INCOMING)
            syncCoreActivity()
            if (TelecomCallManager.isReady() && !deferredTelecom) {
                if (!TelecomCallManager.addIncomingCall(id, peer, name)) end(id, 7)
            } else IncomingCallManager.show(id, channel, peer)
            return
        }
        if (data == null || data.size < 8 || ((type == 37 || type == 38 || type == 40 || type == 42) && data.size < 9) ||
            (type == 36 && (data.size != 41 || data[8] != 0.toByte())) || (type == 39 && data.size != 24) ||
            (type == 44 && (data.size != 17 || (data[8].toInt() and 0xFF) > 3))) { badEvent(type, data); return }
        val id = ByteBuffer.wrap(data, 0, 8).order(ByteOrder.LITTLE_ENDIAN).long
        if (type in listOf(37, 38, 40)) pendingEnds.value = pendingEnds.value - id
        val st = _state.value
        if (st == null || st.callId != id || !st.ongoing) {
            LogManager.addLog("DEBUG", "Call", "ignore event=$type id=$id current=${st?.callId} phase=${st?.phase}")
            return
        }
        when (type) {
            66 -> if (st.phase == CallPhase.OUTGOING) {
                CallRingback.stop()
                _state.value = st.copy(phase = CallPhase.WAIT_DECISION)
                service?.updateNotification()
                LogManager.addLog("INFO", "Call", "Phone busy; waiting for caller choice id=$id")
            }
            35 -> if (st.phase == CallPhase.OUTGOING) {
                _state.value = st.copy(phase = CallPhase.RINGING)
                CallRingback.startRinging()
                service?.updateNotification()
            }
            36 -> {
                if (st.phase == CallPhase.ACTIVE) return
                val peer = MemberFilesProtocol.addressHex(data.copyOfRange(9, 41))
                if (peer.isEmpty()) { badEvent(type, data); return }
                CallRingback.stop()
                IncomingCallManager.stopRinging()
                _state.value = st.copy(peerAddress = peer)
                prepareMedia(id) {
                // Telecom переводит аудиополитику в VoIP до создания AudioRecord/AudioTrack.
                TelecomCallManager.setActive(id)
                val owner = service
                if (owner == null || !owner.startAudio(id)) {
                    LogManager.addLog("ERROR", "Call", "accepted without working audio/service id=$id")
                    end(id, 7)
                    return@prepareMedia
                }
                accepting = false
                _video.value = owner.video
                val name = if (peer == st.peerAddress && !st.devices) st.peerName else ChatRepository().getMembers(st.channelId)
                    .firstOrNull { it.ed25519 == peer }?.name?.takeIf { it.isNotEmpty() } ?: peer
                val current = _state.value ?: return@prepareMedia
                _state.value = current.copy(phase = CallPhase.ACTIVE, peerAddress = peer, peerName = name,
                    durationSeconds = 0, preparing = false, answering = false, startedAtElapsedRealtime = SystemClock.elapsedRealtime())
                owner.audio.router.onChanged = { route, headset -> routeChanged(route, headset) }
                _earpiece.value = owner.audio.router.hasEarpiece
                routeChanged(owner.audio.router.route, owner.audio.router.headsetAvailable)
                owner.video?.let { engine ->
                    videoJob = scope.launch {
                        combine(engine.state, engine.display) { video, display -> video.settings.enabled || display != null }
                            .distinctUntilChanged().collect { active ->
                                if (service !== owner || _state.value?.callId != id || _state.value?.phase != CallPhase.ACTIVE) return@collect
                                if (active == _videoActive.value) return@collect
                                _videoActive.value = active
                                videoSpeakerPending = active && _route.value == AudioRoute.EARPIECE
                                LogManager.addLog("INFO", "call", "video active=$active speaker_pending=$videoSpeakerPending proximity=${proximity.near.value} id=$id")
                                syncProximity(); applyVideoSpeakerIfReady()
                            }
                    }
                }
                owner.updateNotification()
                LogManager.addLog("INFO", "Call", "active id=$id; service owns audio and CPU wake lock")
                }
            }
            37, 38 -> finish(id, data[8].toInt() and 0xFF)
            40 -> {
                LogManager.addLog("ERROR", "Call", "native error id=$id code=${data[8]} ${String(data, 9, data.size - 9)}")
                finish(id, 7)
            }
            39 -> if (st.phase == CallPhase.ACTIVE) {
                val buf = ByteBuffer.wrap(data, 8, 16).order(ByteOrder.BIG_ENDIAN)
                fun metric() = buf.short.toInt() and 0xFFFF
                _state.value = st.copy(stats = CallStats(metric(), metric(), metric(), metric(), metric(), metric(), metric(), metric(),
                    service?.audio?.outputLatency(id) ?: CallOutputLatency()))
            }
            42 -> _state.value = st.copy(bgpPath = String(data, 8, data.size - 8))
            44 -> {
                val connection = CallConnectionType.values()[data[8].toInt() and 0xFF]
                val nextHop = ByteBuffer.wrap(data, 9, 8).order(ByteOrder.LITTLE_ENDIAN).long
                _state.value = st.copy(connectionType = connection, connectionNextHop = nextHop)
                LogManager.addLog("INFO", "Call", "connection id=$id type=$connection via=${nextHop.toULong().toString(16)}")
            }
        }
    }

    private fun badEvent(type: Int, data: ByteArray?) {
        LogManager.addLog("WARN", "Call", "invalid event=$type size=${data?.size ?: 0}")
    }

    override fun onAnswer(callId: Long) {
        val pending = _state.value
        if (pending?.callId == callId && pending.phase == CallPhase.PENDING_INCOMING)
            _state.value = pending.copy(phase = CallPhase.INCOMING)
        if (_state.value?.callId != callId || _state.value?.phase != CallPhase.INCOMING) return
        if (activityVisible) { accept(); return }
        // Ответ гарнитурой в фоне: сначала видимый UI, затем разрешённый запуск microphone FGS.
        answerWhenVisible = callId
        try {
            ChatApplication.instance.startActivity(Intent(ChatApplication.instance, MainActivity::class.java).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                putExtra(IncomingCallManager.EXTRA_CALL_ID, callId)
            })
            LogManager.addLog("INFO", "Call", "Telecom answer id=$callId; waiting for visible Activity before microphone service")
        } catch (e: Exception) {
            answerWhenVisible = 0L
            LogManager.addLog("ERROR", "Call", "cannot show incoming UI for Telecom answer id=$callId: ${e.message}")
        }
    }
    override fun onReject(callId: Long) { end(callId, 2) }
    override fun onDisconnect(callId: Long) { end(callId) }

    override fun onIncomingUiAllowed(callId: Long) {
        val st = _state.value ?: return
        if (st.callId != callId || st.phase != CallPhase.PENDING_INCOMING) return
        _state.value = st.copy(phase = CallPhase.INCOMING)
        LogManager.addLog("INFO", "Call", "Telecom permits incoming UI id=$callId")
        st.peerAddress?.let { IncomingCallManager.show(callId, st.channelId, it) }
    }

    override fun onConnectionFailed(callId: Long) {
        LogManager.addLog("WARN", "Call", "Telecom cannot handle call id=$callId")
        end(callId, 7)
    }

    fun continueCall(id: Long) {
        checkMain()
        val st = _state.value ?: return
        if (st.callId != id || st.phase != CallPhase.WAIT_DECISION) return
        LogManager.addLog("INFO", "Call", "Caller overrides phone busy id=$id")
        if (!NativeLib.callContinue(id)) {
            LogManager.addLog("ERROR", "Call", "continue queue rejected id=$id; caller choice remains available")
            return
        }
        _state.value = st.copy(phase = CallPhase.OUTGOING)
        CallRingback.startConnecting()
        service?.updateNotification()
    }

    private fun checkMain() = check(Looper.myLooper() == Looper.getMainLooper()) { "CallController requires main thread" }
}
