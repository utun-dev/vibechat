package com.utun.chat.data

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioRouting
import android.media.AudioTrack
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.utun.chat.ChatApplication
import com.utun.chat.R
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.TimeUnit

enum class MediaAudioProfile { AUTO, PHONE, WIRED, BLUETOOTH }

enum class MediaAudioNotice(val message: Int, val isError: Boolean) {
    FALLBACK(R.string.media_audio_fallback, false),
    ROUTE_FAILED(R.string.media_audio_failed, true)
}

data class MediaAudioState(
    val preferred: MediaAudioProfile = MediaAudioProfile.AUTO, val active: MediaAudioProfile = MediaAudioProfile.PHONE,
    val wiredAvailable: Boolean = false, val bluetoothAvailable: Boolean = false,
    val inputName: String? = null, val outputName: String? = null, val switching: Boolean = false, val notice: MediaAudioNotice? = null
)

/** Единственная app-scoped сессия общего аудио рации и трансляций; живёт до завершения процесса. */
internal object MediaAudioController : MediaAudioSession()

/**
 * Владелец общего аудио: router, аппаратный reader, AudioTrack leases и один таймер подтверждения.
 * Все изменения оборудования выполняет единственный io worker; подписки PCM защищены inputLock.
 * refresh/select асинхронны. attachOutput/close lease/awaitIdle/close блокируют вызывающего worker,
 * поэтому main и io для них запрещены. Track регистрируется до play и отсоединяется до release.
 * Ошибки инвалидируют поколение; следующий refresh повторяет попытку, сохраняя действующие PCM-подписки.
 * Настройка становится committed только после успешной записи. close требует отсутствия потребителей,
 * запрещает новых и закрывает executor лишь после подтверждения hardware/router cleanup; отказ допускает повтор close.
 * Неудавшееся удаление listener сохраняет отдельную неактивную регистрацию и timer retry, также без выданного lease.
 */
internal open class MediaAudioSession(private val io: ScheduledExecutorService =
    Executors.newSingleThreadScheduledExecutor { Thread(it, "media-audio-control").apply { isDaemon = true } }) : AutoCloseable {
    private val app get() = ChatApplication.instance
    private val am = app.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var controlThread: Thread? = null
    @Volatile private var closed = false
    @Volatile private var closing = false
    private val router = CallAudioRouter()
    private val mutable = MutableStateFlow(MediaAudioState())
    val state: StateFlow<MediaAudioState> = mutable
    private val inputLock = Any()
    private var shared: MicrophoneCapture? = null // Только под inputLock.
    private var bridge: InputBridge? = null // Только под inputLock.
    private var record: AndroidMicrophoneCapture? = null
    private var camcorderCapture: MicrophoneCapture? = null
    private var inputReady = false
    private var generation = 0L
    private var routeKey: List<Any?>? = null
    private var systemRoute: Pair<MediaAudioProfile, Boolean>? = null
    private var preferred = MediaAudioProfile.AUTO
    private var committed = preferred
    private var preferencesLoaded = false
    private var forcedPhone = false
    private var savePending = false
    private var modeOwned = false
    private class OutputRegistration(val session: Int) {
        lateinit var listener: AudioRouting.OnRoutingChangedListener
        var reported = false
        var deviceId: Int? = null
    }
    private val outputs = linkedMapOf<AudioTrack, OutputRegistration>()
    private val pendingOutputs = linkedMapOf<AudioTrack, OutputRegistration>() // Неактивные listeners до SDK ACK, callback не читает track.
    private var outputCleanup: ScheduledFuture<*>? = null
    private var outputCleanupDelay = 250L
    private var confirmationTimeout: ScheduledFuture<*>? = null
    private var wantedOutput: AudioDeviceInfo? = null
    private var wantedInputName: String? = null
    private var routeAvailable = false
    private var availability: Pair<Boolean, Boolean>? = null

    init {
        // Наблюдение не занимает микрофон и не меняет глобальный маршрут без потребителей.
        router.fallbackRoute = AudioRoute.SPEAKER
        router.setGlobalRouting(false)
        // Собственный apply учитывается текущей транзакцией; внешние события возвращаются на io.
        val routeChanged = { if (Thread.currentThread() !== controlThread) refresh() }
        router.onChanged = { _, _ -> routeChanged() }
        router.onInputDevicesChanged = { _, _ -> routeChanged() }
        router.onInputDevice = { routeChanged() }
        router.onOutputDevice = { routeChanged() }
        enqueue {
            controlThread = Thread.currentThread()
            reconcile()
        }
    }

    fun refresh() {
        enqueue { reconcile() }
    }

    fun select(profile: MediaAudioProfile) {
        enqueue {
            if (CallController.blocksMedia()) {
                LogManager.addLog("WARN", "media_audio", "selection blocked by call profile=$profile"); return@enqueue
            }
            loadPreferences()
            preferred = profile; forcedPhone = false; savePending = true
            mutable.value = mutable.value.copy(notice = null)
            LogManager.addLog("INFO", "media_audio", "profile requested=$profile")
            router.reapply()
            reconcile(force = true)
        }
    }

    /** Не открывает микрофон из UI. Вызывается реальным владельцем захвата с RECORD_AUDIO. */
    fun subscribe(name: String): MicrophoneCapture.Subscription = synchronized(inputLock) {
        check(!closed && !closing) {
            LogManager.addLog("ERROR", "media_audio", "capture rejected consumer=$name closing=$closing closed=$closed")
            "Media audio session closing/closed"
        }
        check(!CallController.blocksMedia()) { "Media capture blocked by call" }
        if (shared?.closed != false) {
            val device = InputBridge()
            bridge = device
            shared = MicrophoneCapture(device, { level, message -> LogManager.addLog(level, "media_audio", message) },
                released = { refresh() }, changed = { refresh() })
        }
        shared!!.subscribe(name)
    }

    /** Регистрация завершается до play, удаление — до release. Не вызывается из main или io. */
    fun attachOutput(track: AudioTrack): AutoCloseable {
        checkBlockingCaller()
        check(!closed && !closing) {
            LogManager.addLog("ERROR", "media_audio", "output registration rejected closing=$closing closed=$closed")
            "Media audio session closing/closed"
        }
        val registration = io.submit<OutputRegistration> {
            check(!closed && !closing) {
                LogManager.addLog("ERROR", "media_audio", "queued output registration rejected closing=$closing closed=$closed")
                "Media audio session closing/closed"
            }
            check(!CallController.blocksMedia()) { "Media output blocked by call" }
            check(track !in outputs && track !in pendingOutputs) { "AudioTrack already registered or cleanup pending" }
            val owner = OutputRegistration(track.audioSessionId)
            owner.listener = AudioRouting.OnRoutingChangedListener {
                enqueue {
                    if (outputs[track] !== owner) return@enqueue
                    val actual = track.routedDevice
                    if (owner.reported && owner.deviceId == actual?.id) return@enqueue
                    owner.reported = true; owner.deviceId = actual?.id
                    LogManager.addLog("INFO", "media_audio", "output routed session=${track.audioSessionId} " +
                        "device=${actual?.id}/${actual?.type} mode=${am.mode} sco=${am.isBluetoothScoOn}")
                    reconcile()
                }
            }
            outputs[track] = owner
            try {
                track.addOnRoutingChangedListener(owner.listener, main)
                reconcile()
                check(track.setPreferredDevice(wantedOutput)) { "Output selection rejected session=${track.audioSessionId}" }
                mutable.value = mutable.value.copy(switching = true)
                ensureTimeout()
                confirm()
            } catch (e: Exception) {
                try { detachOutput(track, owner) } catch (cleanup: Exception) {
                    e.addSuppressed(cleanup)
                    LogManager.addLog("ERROR", "media_audio", "output listener cleanup failed session=${owner.session}; retained: $cleanup")
                }
                LogManager.addLog("ERROR", "media_audio", "output registration failed session=${track.audioSessionId}: $e")
                recover(e)
                throw e
            }
            LogManager.addLog("INFO", "media_audio", "output attached session=${track.audioSessionId} count=${outputs.size}")
            owner
        }.get()
        return AutoCloseable {
            if (closed) return@AutoCloseable
            checkBlockingCaller()
            io.submit {
                if (outputs[track] !== registration && pendingOutputs[track] !== registration) return@submit
                var failure: Exception? = null
                try { detachOutput(track, registration) } catch (e: Exception) { failure = e }
                try { reconcile() }
                catch (e: Exception) { recover(e) }
                failure?.let { throw it }
                LogManager.addLog("INFO", "media_audio", "output detached session=${registration.session} count=${outputs.size}")
            }.get()
        }
    }

    private fun detachOutput(track: AudioTrack, owner: OutputRegistration) {
        outputs.remove(track); pendingOutputs[track] = owner
        try { track.removeOnRoutingChangedListener(owner.listener); pendingOutputs.remove(track) } catch (e: Exception) {
            LogManager.addLog("ERROR", "media_audio", "output listener removal failed session=${owner.session}; retained: $e")
            scheduleOutputCleanup(); throw e
        }
        if (pendingOutputs.isEmpty()) { outputCleanup?.cancel(false); outputCleanup = null; outputCleanupDelay = 250 }
    }

    /** Также очищает listener частично неудавшегося attach, у которого caller не получил lease. */
    private fun cleanupOutputListeners() {
        var failure: Exception? = null
        for ((track, owner) in pendingOutputs.toMap()) try { detachOutput(track, owner) } catch (e: Exception) { failure = e }
        failure?.let { throw it }
    }

    private fun scheduleOutputCleanup() {
        if (outputCleanup != null) return
        val delay = outputCleanupDelay
        try {
            outputCleanup = io.schedule({
                outputCleanup = null
                try { cleanupOutputListeners() } catch (e: Exception) {
                    LogManager.addLog("WARN", "media_audio", "output listener cleanup pending count=${pendingOutputs.size}: $e")
                }
            }, delay, TimeUnit.MILLISECONDS)
            outputCleanupDelay = (delay * 2).coerceAtMost(30_000)
            LogManager.addLog("DEBUG", "media_audio", "output listener cleanup scheduled delayMs=$delay count=${pendingOutputs.size}")
        } catch (e: RejectedExecutionException) {
            LogManager.addLog("ERROR", "media_audio", "output listener cleanup rejected; owners retained count=${pendingOutputs.size}: $e")
        }
    }

    /** Барьер cleanup после остановки всех движков, перед передачей ресурсов Telecom. */
    fun awaitIdle() {
        checkBlockingCaller()
        io.submit {
            check(!closing) {
                LogManager.addLog("ERROR", "media_audio", "idle barrier rejected: session cleanup incomplete")
                "Media audio session cleanup incomplete"
            }
            check(outputs.isEmpty() && synchronized(inputLock) { shared?.closed != false }) { "Media audio still owned" }
            cleanupOutputListeners()
            invalidateAttempt()
            releaseHardware()
            mutable.value = mutable.value.copy(switching = false, inputName = null, outputName = null)
            LogManager.addLog("INFO", "media_audio", "all media audio resources released for call")
        }.get()
    }

    override fun close() {
        if (closed) return
        checkBlockingCaller()
        try {
            io.submit {
                synchronized(inputLock) {
                    check(outputs.isEmpty() && shared?.closed != false) { "Media audio still owned" }
                    closing = true
                }
                invalidateAttempt()
                var failure: Exception? = null
                try { cleanupOutputListeners() } catch (e: Exception) { failure = e }
                try { releaseHardware() } catch (e: Exception) {
                    LogManager.addLog("ERROR", "media_audio", "session hardware cleanup failed; close remains retryable: $e")
                    failure = e
                }
                try { router.stop() } catch (e: Exception) {
                    LogManager.addLog("ERROR", "media_audio", "session router cleanup failed; close remains retryable: $e")
                    if (failure == null) failure = e
                }
                failure?.let { throw it }
                closed = true
                LogManager.addLog("INFO", "media_audio", "session closed")
            }.get()
        } finally { if (closed) io.shutdown() }
    }

    private fun checkBlockingCaller() = check(Looper.myLooper() != Looper.getMainLooper() && Thread.currentThread() !== controlThread) {
        LogManager.addLog("ERROR", "media_audio", "blocking audio operation requires a consumer worker")
        "Media audio operation cannot block main/control thread"
    }

    private fun enqueue(action: () -> Unit) {
        if (closed || closing) return
        try {
            io.execute { if (!closed && !closing) try { action() } catch (e: Exception) { recover(e) } }
        } catch (e: RejectedExecutionException) {
            if (!closed) LogManager.addLog("ERROR", "media_audio", "control task rejected: $e")
        }
    }

    private fun isWired(type: Int) = type == AudioDeviceInfo.TYPE_WIRED_HEADSET || type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES ||
        type == AudioDeviceInfo.TYPE_USB_HEADSET || type == AudioDeviceInfo.TYPE_USB_DEVICE || type == AudioDeviceInfo.TYPE_USB_ACCESSORY

    private fun isBluetoothMedia(type: Int) = type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
        (Build.VERSION.SDK_INT >= 31 && type == AudioDeviceInfo.TYPE_BLE_HEADSET)

    private fun isBluetooth(type: Int) = type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO || isBluetoothMedia(type)

    private fun reconcile(force: Boolean = false) {
        loadPreferences()
        router.start()
        val devices = am.getDevices(AudioManager.GET_DEVICES_ALL).toList()
        val wired = devices.any { isWired(it.type) }
        val bluetooth = router.btInputAvailable || devices.any { isBluetooth(it.type) }
        val available = wired to bluetooth
        if (availability == null || availability?.let { (!it.first && wired) || (!it.second && bluetooth) } == true) forcedPhone = false
        availability = available
        val resolved = when (preferred) {
            MediaAudioProfile.AUTO -> if (wired) MediaAudioProfile.WIRED else if (bluetooth) MediaAudioProfile.BLUETOOTH else MediaAudioProfile.PHONE
            MediaAudioProfile.WIRED -> if (wired) preferred else MediaAudioProfile.PHONE
            MediaAudioProfile.BLUETOOTH -> if (bluetooth) preferred else MediaAudioProfile.PHONE
            else -> preferred
        }
        val active = if (forcedPhone) MediaAudioProfile.PHONE else resolved
        val (capture, targetBridge) = synchronized(inputLock) {
            val current = shared?.takeUnless { it.closed }
            current to bridge?.takeIf { current != null }
        }
        // После подключения эфира сохраняем CAMCORDER до последней подписки, без разрыва оставшейся рации.
        if (capture == null || capture.names.contains("stream")) camcorderCapture = capture
        val publishing = capture != null && capture === camcorderCapture
        val stereo = publishing || app.configProvider.getRadioCaptureStereo()
        val wiredSink = devices.any { it.isSink && isWired(it.type) }
        val configuredSource = app.configProvider.getRadioMicSource()
        val micSource = if (publishing) 3 else if (active == MediaAudioProfile.WIRED && !wiredSink && configuredSource == 0) 1
            else configuredSource
        // SCO-only гарнитуре разговорный тракт нужен и при одном playback, после отпускания PTT.
        // При доступном A2DP/LE media-выходе возвращаем качественный media-тракт без микрофона.
        val bluetoothMediaSink = devices.any { it.isSink && isBluetoothMedia(it.type) }
        // Разговорный выход рации владеет трактом и между PTT: повторный MODE_NORMAL → IN_COMMUNICATION
        // заново запускает VoIP HAL. Микрофон по-прежнему принадлежит только реальным подписчикам.
        val voicePlayback = outputs.keys.any { it.audioAttributes.usage == AudioAttributes.USAGE_VOICE_COMMUNICATION }
        val voice = (active == MediaAudioProfile.BLUETOOTH && (capture != null || !bluetoothMediaSink)) ||
            (capture != null && active == MediaAudioProfile.WIRED && wiredSink) ||
            ((capture != null || voicePlayback) && active == MediaAudioProfile.PHONE && !stereo && micSource == 0)
        val owned = capture != null || outputs.isNotEmpty()
        val route = active to (voice && owned)
        if (systemRoute != route) {
            // Смена глобального тракта принадлежит только этому владельцу, до открытия нового reader.
            invalidateAttempt()
            releaseHardware()
            router.setAutoHeadset(false)
            router.setRoute(if (active == MediaAudioProfile.PHONE) AudioRoute.SPEAKER else AudioRoute.HEADSET,
                preferWired = active == MediaAudioProfile.WIRED)
            if (voice && owned) {
                modeOwned = true
                am.mode = AudioManager.MODE_IN_COMMUNICATION
                router.setGlobalRouting(true)
            }
            systemRoute = route
        }
        val output = router.currentOutputDevice
        val input = when (active) {
            MediaAudioProfile.PHONE -> router.builtinMicDevice()
            MediaAudioProfile.WIRED -> router.wiredInputDevice() ?: router.builtinMicDevice()
            else -> router.btInputDevice()
        }
        val key = listOf(owned, active, input?.id, output?.id, voice, stereo, micSource, capture)
        mutable.value = mutable.value.copy(preferred = preferred, active = active, wiredAvailable = wired,
            bluetoothAvailable = bluetooth, inputName = if (capture == null) null else mutable.value.inputName,
            notice = when {
                mutable.value.notice == MediaAudioNotice.ROUTE_FAILED -> MediaAudioNotice.ROUTE_FAILED
                preferred != resolved && preferred != MediaAudioProfile.AUTO -> MediaAudioNotice.FALLBACK
                else -> null
            })
        if (!force && key == routeKey) { confirm(); return }
        invalidateAttempt()
        val epoch = generation
        inputReady = capture == null
        record?.close(); record = null
        mutable.value = mutable.value.copy(switching = owned)
        if (!owned) {
            wantedOutput = null
            mutable.value = mutable.value.copy(switching = false, inputName = null, outputName = null)
            saveSelection()
            routeKey = key
            return
        }
        LogManager.addLog("INFO", "media_audio", "apply generation=$epoch preferred=$preferred active=$active " +
            "input=${input?.id}/${input?.type} output=${output?.id}/${output?.type} voice=$voice voicePlayback=$voicePlayback " +
            "stereo=$stereo users=${capture?.names} outputs=${outputs.size}")
        wantedOutput = output
        wantedInputName = if (capture != null) input?.productName?.toString() else null
        routeAvailable = output != null && (active != MediaAudioProfile.BLUETOOTH || isBluetooth(output.type))
        for (track in outputs.keys) check(track.setPreferredDevice(output)) { "Output rejected session=${track.audioSessionId} device=${output?.id}" }
        if (capture != null && input != null) {
            record = AndroidMicrophoneCapture.open(input, voice, stereo, micSource, epoch,
                accept = { frame -> targetBridge?.offer(frame) == true },
                failed = { error -> enqueue { if (generation == epoch) recover(error) } },
                confirmed = { enqueue { if (generation == epoch) { inputReady = true; confirm() } } })
        } else if (capture != null && active != MediaAudioProfile.BLUETOOTH) error("Selected microphone unavailable profile=$active")
        routeKey = key
        ensureTimeout()
        confirm()
    }

    private fun confirm() {
        if (!mutable.value.switching) return
        if (!inputReady || !outputsReady(outputs.keys) || !routeAvailable) return
        mutable.value = mutable.value.copy(switching = false, inputName = wantedInputName,
            outputName = wantedOutput?.productName?.toString())
        saveSelection()
        confirmationTimeout?.cancel(false); confirmationTimeout = null
        LogManager.addLog("INFO", "media_audio", "route confirmed generation=$generation mode=${am.mode} sco=${am.isBluetoothScoOn} state=${mutable.value}")
    }

    private fun outputsReady(tracks: Collection<AudioTrack>): Boolean = tracks.all { track ->
        val actual = if (track in outputs) track.routedDevice else null
        track !in outputs || (actual != null && actual.id == wantedOutput?.id)
    }

    private fun saveSelection() {
        if (!savePending) return
        kotlinx.coroutines.runBlocking { app.configProvider.setMediaAudioProfile(preferred) }
        savePending = false; committed = preferred
        LogManager.addLog("INFO", "media_audio", "profile saved=$committed")
    }

    private fun loadPreferences() {
        if (preferencesLoaded) return
        preferred = kotlinx.coroutines.runBlocking { app.configProvider.getMediaAudioProfile() }
        committed = preferred; preferencesLoaded = true
        LogManager.addLog("INFO", "media_audio", "profile loaded=$preferred")
    }

    private fun invalidateAttempt() {
        confirmationTimeout?.cancel(false); confirmationTimeout = null
        ++generation
        routeKey = null; inputReady = false; routeAvailable = false
        synchronized(inputLock) { bridge }?.reset(generation)
    }

    private fun ensureTimeout() {
        if (confirmationTimeout != null || !mutable.value.switching) return
        val epoch = generation
        lateinit var task: ScheduledFuture<*>
        task = io.schedule({
            if (closed || closing || generation != epoch || confirmationTimeout !== task) return@schedule
            confirmationTimeout = null
            try {
                check(inputReady && routeAvailable && outputsReady(outputs.keys)) {
                    "Audio route confirmation timeout generation=$epoch inputReady=$inputReady " +
                        "wantedOutput=${wantedOutput?.id} state=${mutable.value}"
                }
                confirm()
            } catch (e: Exception) { recover(e) }
        }, if (mutable.value.active == MediaAudioProfile.BLUETOOTH) 30 else 6, TimeUnit.SECONDS)
        confirmationTimeout = task
    }

    private fun releaseHardware() {
        var failure: Exception? = null
        try { record?.close(); record = null } catch (e: Exception) {
            LogManager.addLog("ERROR", "media_audio", "capture release failed generation=$generation: $e")
            failure = e
        }
        try { router.setGlobalRouting(false) } catch (e: Exception) {
            LogManager.addLog("ERROR", "media_audio", "global route release failed: $e")
            if (failure == null) failure = e
        }
        if (modeOwned) try { am.mode = AudioManager.MODE_NORMAL; modeOwned = false } catch (e: Exception) {
            LogManager.addLog("ERROR", "media_audio", "audio mode release failed: $e")
            if (failure == null) failure = e
        }
        systemRoute = null
        failure?.let { throw it }
    }

    private fun recover(error: Exception) {
        LogManager.addLog("ERROR", "media_audio", "route failed generation=$generation preferred=$preferred: $error")
        if (savePending) { preferred = committed; savePending = false }
        val fallback = !forcedPhone
        forcedPhone = true
        mutable.value = mutable.value.copy(preferred = preferred, notice = MediaAudioNotice.ROUTE_FAILED,
            switching = false, inputName = null, outputName = null)
        invalidateAttempt()
        try {
            releaseHardware()
            if (fallback) reconcile(force = true)
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "media_audio", "fallback failed generation=$generation: $e")
            invalidateAttempt()
            try { releaseHardware() } catch (cleanup: Exception) {
                LogManager.addLog("ERROR", "media_audio", "fallback cleanup failed: $cleanup")
            }
            mutable.value = mutable.value.copy(switching = false, inputName = null, outputName = null)
        }
    }

    /** Постоянный stereo-вход с запасом времени на аппаратный reader и смену маршрута. */
    private class InputBridge : MicrophoneCapture.Device {
        private val lock = Object()
        private val timeline = MediaCaptureTimeline(System.nanoTime()) { level, message -> LogManager.addLog(level, "media_audio", message) }
        private var running = true
        private var reportedLate = 0L
        private var reportedMissing = 0L
        private var reportNs = 0L
        override val channels = 2
        override val inputId: Int? get() = null
        fun reset(epoch: Long) = synchronized(lock) { timeline.reset(epoch) }
        fun offer(frame: MicrophoneCapture.Frame): Boolean = synchronized(lock) {
            val accepted = timeline.offer(frame)
            if (timeline.lateFrames / 50 != reportedLate / 50 || (timeline.lateFrames > 0 && reportedLate == 0L)) {
                reportedLate = timeline.lateFrames
                LogManager.addLog("WARN", "media_audio", "late microphone PCM blocks=$reportedLate generation=${timeline.generation}")
            }
            accepted
        }
        override fun read(): MicrophoneCapture.Frame = synchronized(lock) {
            while (running) {
                val now = System.nanoTime()
                val wait = timeline.deadlineNs - now
                if (wait <= 0) {
                    val frame = timeline.pull(now)
                    if (now - reportNs >= 5_000_000_000L || (reportedMissing == 0L && timeline.missingFrames > 0)) {
                        val level = if (timeline.missingFrames > reportedMissing) "WARN" else "DEBUG"
                        LogManager.addLog(level, "media_audio", "PCM clock generation=${timeline.generation} " +
                            "jitterMaxUs=${timeline.timestampJitterMaxNs / 1000} jumps=${timeline.timestampJumps} ratePpm=${timeline.ratePpm} " +
                            "missingFrames=${timeline.missingFrames} silenceFrames=${timeline.silentFrames} lateBlocks=${timeline.lateFrames}")
                        reportedMissing = timeline.missingFrames; reportNs = now
                    }
                    return@synchronized frame
                }
                lock.wait(wait / 1_000_000, (wait % 1_000_000).toInt())
            }
            error("Shared microphone stopped")
        }
        override fun stop() = synchronized(lock) { running = false; lock.notifyAll() }
        override fun close() {}
    }
}
