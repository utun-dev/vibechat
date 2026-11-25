package com.utun.chat.data

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioRouting
import android.media.AudioTimestamp
import android.media.AudioTrack
import android.media.MediaRecorder
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock
import com.utun.chat.ChatApplication

/**
 * Android I/O P2P-звонка: PCM 48 кГц mono, блоки по 960 сэмплов (20 мс).
 *
 * Владелец вызывает start/stop и меняет маршрут на main; start возвращает результат запуска
 * native-стека и подготовки workers, готовность устройств наступает асинхронно.
 * onFailure приходит на main для ещё активного звонка; владелец завершает его и вызывает stop.
 *
 * Capture worker собирает полный блок AudioRecord и передаёт настоящий микрофон, mute и PTS
 * первого сэмпла в native AEC/AGC/Opus. Playback worker получает финальный PCM из call_audio
 * (адаптивный Opus JB, SoundTouch, тоны) и полностью пишет блок до следующего pull.
 * Выход — AAudio для встроенного speaker на API 31+ при успешной проверке native backend,
 * иначе AudioTrack. Темп задаёт устройство; вне него ожидает не более одного блока PCM.
 *
 * Reporter раз в 200 мс снимает аппаратный timestamp и регулирует размер буфера устройства
 * независимо от уровня логов. Аппаратные часы задают время вывода конца записанного PCM;
 * оценка по playback head допустима лишь до первого принятого timestamp за этот запуск.
 * При смене выхода сохраняются native JB и общий счётчик PCM, его старый аппаратный якорь снимается.
 * Stop ждёт capture/playback и вложенный reporter до освобождения устройств и native-стека.
 * При незавершённой очистке сохраняет владельцев и бросает ошибку; CallService повторяет stop,
 * а новый start сначала обязан завершить прежний stop.
 */
class CallAudioEngine {

    companion object {
        const val SAMPLE_RATE = 48000
        const val FRAME_MS = 20
        const val FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000   // 960

        /** Задержка рендер→захват AEC по умолчанию (кадров), если замер недоступен. */
        const val AEC_DEFAULT_DELAY_FRAMES = 4
        /** Акустический ход эха ~3мс (сэмплы @48кГц), добавляется к замеру. */
        const val AEC_ACOUSTIC_SAMPLES = 150L
    }

    private val recordLock = ReentrantLock()
    private val trackLock = ReentrantLock()
    @Volatile private var audioRecord: AudioRecord? = null
    private var captureThread: Thread? = null
    @Volatile private var audioTrack: CallAudioOutput? = null
    private var playThread: Thread? = null
    private var prevMode: Int = AudioManager.MODE_NORMAL
    private var prevSpeaker: Boolean = false
    private var modeOwned = false // Main: прежний AudioManager mode/speaker ещё надо восстановить.

    /** Маршрутизация аудио-выхода (динамик/громкая/гарнитура). */
    val router = CallAudioRouter()

    @Volatile private var active = false
    @Volatile private var callId = 0L
    @Volatile private var captureReady = false
    @Volatile private var playReady = false
    private var duplexReady = false // только main

    @Volatile private var muted = false

    /* Захват всегда VOICE_COMMUNICATION; без Telecom проводной HEADSET дополнительно
     * выбирает wired-микрофон через preferredDevice. captureLoop пересоздаёт запись при смене wired-режима. */
    @Volatile private var wantedWired = false
    @Volatile private var wiredActive = false

    /* Для замера задержки AEC (getTimestamp) — счётчики кадров play/capture. */
    @Volatile private var playFramesWritten = 0L
    @Volatile private var playFramesPlayed = 0L
    @Volatile private var capFramesRead = 0L
    private var playbackBuffer: CallPlaybackBuffer? = null // под trackLock
    @Volatile private var routeGeneration = 0L
    @Volatile private var confirmedDeviceId = 0
    private var communicationListener: AudioManager.OnCommunicationDeviceChangedListener? = null
    private val outputPosition = CallOutputPosition() // writer; reporter читает под trackLock
    private var outputRecovery = CallOutputRecovery()
    private data class OutputSample(val atUs: Long, val generation: Long, val latency: CallOutputLatency)
    @Volatile private var outputSample: OutputSample? = null
    @Volatile private var startElapsedMs = 0L
    @Volatile private var captureElapsedMs = 0L
    private val mainHandler = Handler(Looper.getMainLooper())
    private val trackRouteListener = AudioRouting.OnRoutingChangedListener { output ->
        withDeviceLock(trackLock) {
            val track = audioTrack
            if (active && track != null && output === track.track) try {
                val device = track.routedDevice
                if (Build.VERSION.SDK_INT < 31) confirmedDeviceId = device?.id ?: 0
                LogManager.addLog("INFO", "CallAudio", "playback routed id=$callId backend=${track.name} " +
                    "device=${device?.id}/${device?.type} communication=$confirmedDeviceId selected=${router.route}")
            } catch (e: Exception) {
                LogManager.addLog("WARN", "CallAudio", "playback routing diagnostic failed id=$callId: $e")
            }
        }
    }
    private val recordRouteListener = AudioRouting.OnRoutingChangedListener { input ->
        withDeviceLock(recordLock) {
            val record = audioRecord
            if (active && record != null && input === record) try {
                val device = record.routedDevice
                LogManager.addLog("INFO", "CallAudio", "capture routed id=$callId device=${device?.id}/${device?.type} " +
                    "preferred=${record.preferredDevice?.id} communication=$confirmedDeviceId selected=${router.route} wired=$wiredActive")
            } catch (e: Exception) {
                LogManager.addLog("WARN", "CallAudio", "capture routing diagnostic failed id=$callId: $e")
            }
        }
    }
    private val aecDelayRunnable = Runnable { measureAecDelay() }
    private val failed = AtomicBoolean(false)
    /** Первый сбой worker: call ID и причина на main. Владелец должен остановить/завершить звонок. */
    var onFailure: ((Long, String) -> Unit)? = null

    fun isActive(): Boolean = active

    /** Снимок задержки для id, не старше 500 мс и из текущего выхода; неизвестные поля равны -1. */
    fun outputLatency(id: Long): CallOutputLatency {
        val sample = outputSample
        return if (active && id == callId && sample != null && sample.generation == routeGeneration &&
            System.nanoTime() / 1000 - sample.atUs in 0..500_000) sample.latency else CallOutputLatency()
    }

    /** Поздняя Telecom Connection: снять собственные mode/глобальный маршрут, затем передать владение.
     * Отказ передаётся через onFailure; stop сохраняет неподтверждённые ресурсы для повторной очистки. */
    fun bindTelecom(connection: UtunConnection) {
        if (!active || router.telecom === connection) return
        try {
            if (router.telecom == null) {
                val am = ChatApplication.instance.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                am.mode = AudioManager.MODE_NORMAL
                modeOwned = false
            }
            router.bindTelecom(connection)
        } catch (e: Exception) {
            reportFailure(callId, "Telecom routing handoff failed: $e")
        }
    }

    val diagnostics: String get() =
        "active=$active ready=$captureReady/$playReady capture=$capFramesRead play=$playFramesWritten " +
            "workers=${captureThread?.isAlive}/${playThread?.isAlive} " +
            "recordOwned=${audioRecord != null} trackOwned=${audioTrack != null} route=${router.route}"

    /** SDK-вызовы workers не задерживают main: маршрут/замер повторяется только для того же активного звонка. */
    private fun withDeviceLock(lock: ReentrantLock, work: () -> Unit) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            lock.withLock(work)
        } else if (lock.tryLock()) {
            try { work() } finally { lock.unlock() }
        } else {
            val id = callId
            if (active && !mainHandler.postDelayed({ if (active && callId == id) withDeviceLock(lock, work) }, 50)) {
                LogManager.addLog("ERROR", "CallAudio", "device action retry rejected id=$id")
            }
        }
    }

    /** Mute применяется в native после AEC/AGC; настоящий микрофон продолжает обучать обработку. */
    fun setMuted(m: Boolean) {
        if (muted == m) return
        muted = m
        LogManager.addLog("INFO", "CallAudio", "mute ${if (m) "on" else "off"}")
    }

    fun isMuted(): Boolean = muted

    /** Main: остановить прежний запуск, подготовить маршрут и запустить независимые RX/TX.
     * true — workers запущены; ошибки внутри них приходят через onFailure. false — синхронный отказ старта.
     * Ошибка очистки прежнего/частичного запуска бросается владельцу с сохранёнными ресурсами. id не равен нулю. */
    fun start(id: Long): Boolean {
        check(Looper.myLooper() == Looper.getMainLooper()) {
            LogManager.addLog("ERROR", "CallAudio", "start outside main id=$id")
            "Call audio start requires main"
        }
        require(id != 0L) {
            LogManager.addLog("ERROR", "CallAudio", "start rejected: zero call id")
            "Call id must be nonzero"
        }
        stop()
        startElapsedMs = SystemClock.elapsedRealtime()

        if (!NativeLib.callAudioStart(id)) {
            LogManager.addLog("ERROR", "CallAudio", "native callAudioStart failed id=$id")
            return false
        }
        val nativeMs = SystemClock.elapsedRealtime() - startElapsedMs

        callId = id
        routeGeneration = 0; outputSample = null
        outputRecovery = CallOutputRecovery()
        muted = false
        failed.set(false)
        captureReady = false; playReady = false; duplexReady = false
        try {
            router.bindTelecom(TelecomCallManager.connectionFor(id))
            val ctx = ChatApplication.instance
            val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            prevMode = am.mode
            prevSpeaker = am.isSpeakerphoneOn
            modeOwned = router.telecom == null
            var stageAt = SystemClock.elapsedRealtime()
            try {
                if (router.telecom == null) am.mode = AudioManager.MODE_IN_COMMUNICATION
            } catch (e: Exception) {
                LogManager.addLog("WARN", "CallAudio", "set audio mode failed: ${e.message}")
            }
            val modeMs = SystemClock.elapsedRealtime() - stageAt

            /* Маршрут выхода: автоследование за гарнитурой (воткнули → HEADSET,
             * вынули → сохранённый маршрут, по умолчанию тихая связь). */
            router.fallbackRoute = if (router.preferredRoute == AudioRoute.SPEAKER) AudioRoute.SPEAKER else AudioRoute.EARPIECE
            router.setAutoHeadset(true)
            stageAt = SystemClock.elapsedRealtime()
            router.start()
            val routeMs = SystemClock.elapsedRealtime() - stageAt

            /* AEC: включён только на громкой связи (спикерфон); на гарнитуре/разговорном
             * динамике эхо незначимо. Задержка рендер→захват замеряется чуть позже. */
            playFramesWritten = 0L
            playFramesPlayed = 0L
            capFramesRead = 0L
            syncAecForRoute(router.route)

            wiredActive = router.telecom == null && router.route == AudioRoute.HEADSET && router.wiredInputDevice() != null
            wantedWired = wiredActive
            router.onInputDevice = { _ ->
                wantedWired = router.telecom == null && router.route == AudioRoute.HEADSET && router.wiredInputDevice() != null
            }
            router.onOutputDevice = { dev -> applyTrackOutputDevice(dev) }
            router.onRouteRequested = {
                withDeviceLock(trackLock) { playbackBuffer?.settle(SystemClock.elapsedRealtime()) }
            }

            /* Замер AEC — через 300мс после готовности обоих направлений; RX этого не ждёт. */
            val ready = Runnable {
                if (callId == id && active && !failed.get() && captureReady && playReady && !duplexReady) {
                    duplexReady = true
                    scheduleAecDelay(300)
                    LogManager.addLog("INFO", "CallAudio", "duplex ready id=$id totalMs=${SystemClock.elapsedRealtime() - startElapsedMs}")
                }
            }
            active = true
            confirmedDeviceId = if (Build.VERSION.SDK_INT >= 31) am.communicationDevice?.id ?: 0 else 0
            if (Build.VERSION.SDK_INT >= 31) {
                val listener = AudioManager.OnCommunicationDeviceChangedListener { device ->
                    if (active && callId == id) {
                        val next = device?.id ?: 0
                        if (next != confirmedDeviceId) {
                            LogManager.addLog("INFO", "CallAudio", "output route confirmed id=$id device=$next type=${device?.type}")
                            confirmedDeviceId = next
                        }
                    }
                }
                communicationListener = listener
                am.addOnCommunicationDeviceChangedListener(ctx.mainExecutor, listener)
            }

            // Каждый поток готовит только своё устройство. Отмена ждёт оба до release Android/native.
            playThread = Thread({ runWorker(id, "play") {
                trackLock.withLock {
                    if (!active) return@runWorker
                    outputPosition.reset()
                    outputRecovery.route(confirmedDeviceId)
                    openPlaybackOutput("start")
                    playReady = true
                    LogManager.addLog("INFO", "CallAudio",
                        "playback started id=$id totalMs=${SystemClock.elapsedRealtime() - startElapsedMs} " +
                            "backend=${audioTrack?.name} captureReady=$captureReady")
                }
                if (!mainHandler.post(ready)) LogManager.addLog("ERROR", "CallAudio", "playback ready callback rejected id=$id")
                playLoop()
            } }, "utun-call-play")
            playThread!!.start()

            captureThread = Thread({ runWorker(id, "capture") {
                recordLock.withLock {
                    if (!active) return@runWorker
                    val createdAt = SystemClock.elapsedRealtime()
                    wiredActive = wantedWired
                    val rec = createAudioRecord(wiredActive)
                    if (!active) return@runWorker
                    if (rec == null) {
                        reportFailure(id, "AudioRecord create failed wired=$wiredActive")
                        return@runWorker
                    }
                    val recordCreateMs = SystemClock.elapsedRealtime() - createdAt
                    captureElapsedMs = SystemClock.elapsedRealtime()
                    rec.startRecording()
                    if (!active) return@runWorker
                    if (rec.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
                        reportFailure(id, "capture not running state=${rec.recordingState}")
                        return@runWorker
                    }
                    captureReady = true
                    LogManager.addLog("INFO", "CallAudio",
                        "capture started id=$id totalMs=${SystemClock.elapsedRealtime() - startElapsedMs} " +
                            "recordCreateMs=$recordCreateMs recordStartMs=${SystemClock.elapsedRealtime() - captureElapsedMs} " +
                            "playReady=$playReady")
                }
                if (!mainHandler.post(ready)) LogManager.addLog("ERROR", "CallAudio", "capture ready callback rejected id=$id")
                captureLoop()
            } }, "utun-call-capture")
            captureThread!!.start()
            LogManager.addLog("INFO", "CallAudio",
                "preparing RX/TX independently id=$id totalMs=${SystemClock.elapsedRealtime() - startElapsedMs} " +
                    "nativeMs=$nativeMs modeMs=$modeMs routeMs=$routeMs")
            return true
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudio", "start failed id=$id: $e")
            stop()
            return false
        }
    }

    /** Перехватываем сбой worker и передаём владельцу на main; старый callback не завершит новый звонок. */
    private fun runWorker(id: Long, name: String, work: () -> Unit) {
        try {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO)
            work()
            if (active) reportFailure(id, "$name worker stopped unexpectedly")
        } catch (e: Exception) {
            if (active) reportFailure(id, "$name worker failed: ${e.message}")
        } finally {
            LogManager.addLog("INFO", "CallAudio", "$name worker exited id=$id active=$active")
        }
    }

    private fun reportFailure(id: Long, detail: String) {
        if (!active || !failed.compareAndSet(false, true)) return
        LogManager.addLog("ERROR", "CallAudio", "$detail id=$id route=${router.route} capture=$capFramesRead play=$playFramesWritten")
        if (!mainHandler.post { if (callId == id && active) onFailure?.invoke(id, detail) }) {
            LogManager.addLog("ERROR", "CallAudio", "failure callback rejected id=$id detail=$detail")
        }
    }

    /** Main: отменить работу, разблокировать read и подтвердить join/release каждой независимой части.
     * Join ограничен 250 мс на worker; живые потоки и устройства остаются до следующей попытки.
     * При неполной очистке бросает ошибку владельцу; native ждёт устройства, router, listener и mode. */
    fun stop() {
        check(Looper.myLooper() == Looper.getMainLooper()) {
            LogManager.addLog("ERROR", "CallAudio", "stop outside main id=$callId")
            "Call audio stop requires main"
        }
        if (callId == 0L) return
        val id = callId
        active = false
        router.onOutputDevice = null
        router.onInputDevice = null
        router.onRouteRequested = null
        mainHandler.removeCallbacks(aecDelayRunnable)
        if (Build.VERSION.SDK_INT >= 31) {
            communicationListener?.let { listener ->
                try {
                    val am = ChatApplication.instance.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                    am.removeOnCommunicationDeviceChangedListener(listener)
                    communicationListener = null
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "CallAudio", "communication listener removal failed id=$id: $e")
                }
            }
        }
        if (recordLock.tryLock()) try {
            audioRecord?.takeIf { captureThread != null }?.let { rec ->
                try { if (rec.recordingState == AudioRecord.RECORDSTATE_RECORDING) rec.stop() }
                catch (e: Exception) { LogManager.addLog("ERROR", "CallAudio", "record stop failed id=$id: $e") }
            }
        } finally { recordLock.unlock() }
        else LogManager.addLog("WARN", "CallAudio", "record stop deferred id=$id; worker holds device lock")
        if (trackLock.tryLock()) try {
            audioTrack?.takeIf { playThread != null }?.let { track ->
                try { track.removeOnRoutingChangedListener(trackRouteListener) }
                catch (e: Exception) { LogManager.addLog("WARN", "CallAudio", "track listener removal failed id=$id: $e") }
                try { if (track.playState == AudioTrack.PLAYSTATE_PLAYING) track.pause() }
                catch (e: Exception) { LogManager.addLog("WARN", "CallAudio", "track pause failed id=$id: $e") }
            }
        } finally { trackLock.unlock() }
        else LogManager.addLog("WARN", "CallAudio", "track pause deferred id=$id; worker holds device lock")
        playThread?.interrupt()
        var interrupted = false
        for (worker in listOfNotNull(captureThread, playThread)) {
            try { worker.join(250) }
            catch (e: InterruptedException) {
                interrupted = true
                LogManager.addLog("WARN", "CallAudio", "worker join interrupted id=$id worker=${worker.name}: $e")
            }
            if (worker.isAlive) LogManager.addLog("ERROR", "CallAudio", "worker join pending id=$id worker=${worker.name} state=${worker.state}")
        }
        if (captureThread?.isAlive != true) captureThread = null
        if (playThread?.isAlive != true) playThread = null
        if (captureThread == null && playThread == null) {
            recordLock.withLock {
                try { audioRecord?.release(); audioRecord = null }
                catch (e: Exception) { LogManager.addLog("ERROR", "CallAudio", "record release failed id=$id: $e") }
            }
            trackLock.withLock {
                try { audioTrack?.release(); audioTrack = null }
                catch (e: Exception) { LogManager.addLog("ERROR", "CallAudio", "track release failed id=$id: $e") }
                playbackBuffer = null
            }
        }
        captureReady = false; playReady = false; duplexReady = false
        try { router.stop() } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudio", "router stop failed id=$id; retained for retry: $e")
        }
        if (modeOwned) {
            try {
                val am = ChatApplication.instance.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                am.isSpeakerphoneOn = prevSpeaker
                am.mode = prevMode
                modeOwned = false
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallAudio", "restore audio mode failed id=$id: $e")
            }
        }
        if (interrupted) Thread.currentThread().interrupt()
        check(captureThread == null && playThread == null && audioRecord == null && audioTrack == null &&
            communicationListener == null && !modeOwned && !router.cleanupPending) {
            val detail = "cleanup pending id=$id workers=${captureThread?.name}/${playThread?.name} " +
                "record=${audioRecord != null} track=${audioTrack != null} listener=${communicationListener != null} " +
                "mode=$modeOwned router=${router.cleanupPending}"
            LogManager.addLog("ERROR", "CallAudio", detail)
            detail
        }
        try { NativeLib.callAudioStop() }
        catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudio", "native stop failed id=$id: $e")
            throw e
        }
        callId = 0L
        muted = false
        LogManager.addLog("INFO", "CallAudio", "stopped id=$id capture=$capFramesRead play=$playFramesWritten; workers joined")
    }

    private fun captureLoop() {
        val buf = ShortArray(FRAME_SAMPLES)
        var rec = audioRecord
        var frame = 0L
        var sinceSwap = 0
        val clock = CallCaptureClock()
        val stamp = AudioTimestamp()
        var loggedMeasured: Boolean? = null
        var reportNs = System.nanoTime()
        var ageLow = Long.MAX_VALUE; var ageHigh = Long.MIN_VALUE; var ageSum = 0L
        var ageCount = 0; var measuredCount = 0; var mutedFrames = 0
        while (active) {
            /* проводная гарнитура появилась/исчезла — меняем источник захвата */
            if (wantedWired != wiredActive) {
                recordLock.withLock {
                    if (!active) return
                    rec?.apply {
                        stop()
                        release()
                    }
                    audioRecord = null
                    wiredActive = wantedWired
                    rec = createAudioRecord(wiredActive)
                    if (rec == null) return
                    rec?.startRecording()
                }
                sinceSwap = 0
                capFramesRead = 0
                clock.reset()
                NativeLib.callAudioResetCapture(callId)
                loggedMeasured = null
            }
            val r = rec ?: return
            if (r.recordingState != AudioRecord.RECORDSTATE_RECORDING) {
                reportFailure(callId, "capture not recording state=${r.recordingState}")
                return
            }
            var read = 0
            val readAt = if (frame == 0L) SystemClock.elapsedRealtime() else 0L
            while (read < FRAME_SAMPLES) {
                if (!active) return
                // stop() разблокирует read до join; обычный захват не требует polling.
                val n = r.read(buf, read, FRAME_SAMPLES - read, AudioRecord.READ_BLOCKING)
                if (n <= 0) {
                    reportFailure(callId, "AudioRecord.read=$n offset=$read state=${r.recordingState}")
                    return
                }
                read += n
            }
            if (frame == 0L) {
                val now = SystemClock.elapsedRealtime()
                LogManager.addLog("INFO", "CallAudio",
                    "first capture id=$callId totalMs=${now - startElapsedMs} " +
                        "recordToFrameMs=${now - captureElapsedMs} readMs=${now - readAt} playReady=$playReady")
            }
            /* диагностика: уровень захваченного сигнала (тишина vs звук) */
            if (sinceSwap < 10 || frame % 100L == 0L) {
                LogManager.addLog("DEBUG", "CallAudio",
                    "cap wired=$wiredActive frame=$frame rmsDb=${rmsDb(buf)} selected=${router.route} " +
                        "communication=$confirmedDeviceId muted=$muted")
            }
            sinceSwap++
            frame++
            val frameMuted = muted // Один snapshot на кадр; AEC/AGC всегда получают настоящий микрофон.
            val stampOk = r.getTimestamp(stamp, AudioTimestamp.TIMEBASE_MONOTONIC) == AudioRecord.SUCCESS
            val ptsUs = clock.firstSampleUs(capFramesRead, FRAME_SAMPLES, System.nanoTime() / 1000,
                if (stampOk) stamp.framePosition else null, if (stampOk) stamp.nanoTime / 1000 else null)
            if (loggedMeasured != clock.measured) {
                loggedMeasured = clock.measured
                LogManager.addLog(if (clock.measured) "INFO" else "WARN", "CallAudio",
                    "capture clock id=$callId measured=${clock.measured} frame=$capFramesRead ptsUs=$ptsUs")
            }
            capFramesRead += FRAME_SAMPLES
            val feedAt = System.nanoTime()
            val ageUs = feedAt / 1000 - ptsUs
            ageLow = minOf(ageLow, ageUs); ageHigh = maxOf(ageHigh, ageUs); ageSum += ageUs; ageCount++
            if (clock.measured) measuredCount++
            if (frameMuted) mutedFrames++
            if (!NativeLib.callAudioFeed(callId, buf, frameMuted, ptsUs)) {
                LogManager.addLog("WARN", "CallAudio", "callAudioFeed failed id=$callId muted=$frameMuted ptsUs=$ptsUs")
            }
            if (feedAt - reportNs >= 1_000_000_000) {
                LogManager.addLog("DEBUG", "call", "audio capture latency id=$callId frames=$ageCount mutedFrames=$mutedFrames " +
                    "measured=$measuredCount firstSampleAgeUs=$ageLow/${ageSum / ageCount}/$ageHigh")
                reportNs = feedAt; ageLow = Long.MAX_VALUE; ageHigh = Long.MIN_VALUE; ageSum = 0
                ageCount = 0; measuredCount = 0; mutedFrames = 0
            }
        }
    }

    /** RMS захваченного кадра в dBFS; при RMS < 1 возвращает -120. */
    private fun rmsDb(buf: ShortArray): Int {
        var sum = 0L
        for (s in buf) sum += s.toLong() * s.toLong()
        val rms = Math.sqrt(sum.toDouble() / buf.size)
        return if (rms < 1.0) -120 else (20.0 * Math.log10(rms / 32768.0)).toInt()
    }

    /** Создать AudioRecord: всегда VOICE_COMMUNICATION (звонковый тракт «запитывает»
     *  микрофон проводной гарнитуры; AudioSource.MIC на ней даёт тишину). Для проводной
     *  гарнитуры дополнительно роутим setPreferredDevice на её микрофон. */
    private fun createAudioRecord(wired: Boolean): AudioRecord? {
        val queryAt = SystemClock.elapsedRealtime()
        val recMin = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val queryMs = SystemClock.elapsedRealtime() - queryAt
        if (recMin <= 0) {
            LogManager.addLog("ERROR", "CallAudio", "AudioRecord.getMinBufferSize=$recMin wired=$wired")
            return null
        }
        val constructorAt = SystemClock.elapsedRealtime()
        val rec = try {
            AudioRecord(
                MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
                (recMin * 2).coerceAtLeast(FRAME_SAMPLES * 4))
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudio", "AudioRecord create failed (wired=$wired): ${e.message}")
            return null
        }
        val constructorMs = SystemClock.elapsedRealtime() - constructorAt
        audioRecord = rec // Под recordLock: владение до любых запросов состояния/регистрации listeners.
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            LogManager.addLog("ERROR", "CallAudio", "AudioRecord not initialized (wired=$wired)")
            return null
        }
        val routeAt = SystemClock.elapsedRealtime()
        if (wired) {
            val dev = router.wiredInputDevice()
            val ok = try {
                rec.setPreferredDevice(dev)
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallAudio", "record setPreferredDevice(wired) threw: ${e.message}")
                false
            }
            LogManager.addLog(if (ok) "INFO" else "WARN", "CallAudio",
                "record input -> VOICE_COMMUNICATION dev=${dev?.type ?: "-"} setPreferredDevice=$ok")
        } else {
            LogManager.addLog("INFO", "CallAudio", "record input -> VOICE_COMMUNICATION (default)")
        }
        LogManager.addLog("INFO", "CallAudio", "capture device ready id=$callId minQueryMs=$queryMs constructorMs=$constructorMs " +
            "routingMs=${SystemClock.elapsedRealtime() - routeAt} minBytes=$recMin")
        rec.addOnRoutingChangedListener(recordRouteListener, mainHandler)
        return rec
    }

    /** Под trackLock; единственный создатель/владелец выхода — playback worker. */
    private fun openPlaybackOutput(reason: String) {
        val am = ChatApplication.instance.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val begin = SystemClock.elapsedRealtime()
        val track = CallAudioOutput.open(am, outputRecovery.deviceId, outputRecovery.tryFast)
        audioTrack = track
        check(track.track == null || track.track.state == AudioTrack.STATE_INITIALIZED) { "AudioTrack not initialized" }
        applyTrackOutputDevice(router.currentOutputDevice)
        configurePlaybackBuffer(track, reason)
        track.addOnRoutingChangedListener(trackRouteListener, mainHandler)
        track.play()
        check(track.playState == AudioTrack.PLAYSTATE_PLAYING) { "${track.name} not playing" }
        LogManager.addLog("INFO", "CallAudio", "output opened id=$callId backend=${track.name} reason=$reason " +
            "device=${outputRecovery.deviceId} generation=$routeGeneration base=${outputPosition.base} " +
            "failures=${outputRecovery.failures} openMs=${SystemClock.elapsedRealtime() - begin}")
    }

    private fun replacePlaybackOutput(epoch: Long, reason: String) {
        trackLock.withLock {
            if (!active) return
            CallOutputNative.suspendClock(callId, epoch)
            routeGeneration++
            outputSample = null
            val old = audioTrack
            LogManager.addLog("INFO", "CallAudio", "output replace id=$callId reason=$reason backend=${old?.name} " +
                "written=$playFramesWritten head=$playFramesPlayed pendingFrames=${playFramesWritten - playFramesPlayed} " +
                "device=${outputRecovery.deviceId} next=$confirmedDeviceId")
            old?.removeOnRoutingChangedListener(trackRouteListener)
            old?.release()
            audioTrack = null
            outputPosition.reopen(playFramesWritten)
            playFramesPlayed = playFramesWritten
            outputRecovery.route(confirmedDeviceId)
            openPlaybackOutput(reason)
        }
    }

    private fun playLoop() {
        val trace = CallAudioTrace(callId) { level, message -> LogManager.addLog(level, "call", message) }
        val reporter = Thread({
            try {
                val stamp = AudioTimestamp()
                while (!Thread.currentThread().isInterrupted) {
                    trackLock.withLock {
                        audioTrack?.let { track ->
                            if (active) {
                                trace.device = samplePlaybackDevice(track, stamp)
                                adjustPlaybackBuffer(track)
                            }
                        }
                    }
                    trace.flush()
                    Thread.sleep(200)
                }
            } catch (_: InterruptedException) {
                // Playback уже закончен; ниже сохраняем неполное окно события.
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallAudio", "playback buffer monitor/diagnostics failed: ${e.message}")
            } finally {
                try { trace.flush(true) }
                catch (e: Exception) { LogManager.addLog("ERROR", "CallAudio", "diagnostic flush failed: ${e.message}") }
            }
        }, "utun-call-diagnostics").apply { start() }
        try { playLoop(trace) }
        finally {
            reporter.interrupt()
            // Android-запрос может не прерываться; release AudioTrack ждёт завершения reporter.
            var interrupted = Thread.interrupted()
            while (reporter.isAlive) {
                try { reporter.join() }
                catch (_: InterruptedException) { interrupted = true }
            }
            if (interrupted) Thread.currentThread().interrupt()
        }
    }

    /** Только reporter: Binder/внутренние locks AudioTrack не должны вызываться здесь из playback. */
    private fun samplePlaybackDevice(track: CallAudioOutput, stamp: AudioTimestamp): CallAudioTrace.DeviceSample {
        val beginNs = System.nanoTime()
        val generation = routeGeneration
        val stampOk = track.getTimestamp(stamp)
        val stampEndNs = System.nanoTime()
        val route = track.routedDevice
        val routeEndNs = System.nanoTime()
        val underruns = track.underrunCount.toLong()
        val bufferFrames = track.bufferSizeInFrames.toLong()
        val startFrames = if (Build.VERSION.SDK_INT >= 31) track.startThresholdInFrames.toLong() else bufferFrames
        val performanceMode = track.performanceMode
        val endNs = System.nanoTime()
        return CallAudioTrace.DeviceSample(
            route?.id?.toLong() ?: -1, route?.type?.toLong() ?: -1, underruns, bufferFrames, startFrames,
            if (stampOk) outputPosition.timestamp(stamp.framePosition, playFramesWritten) else -1,
            if (stampOk) stamp.nanoTime else -1, stampOk,
            beginNs / 1000, endNs / 1000, (stampEndNs - beginNs) / 1000,
            (routeEndNs - stampEndNs) / 1000, (endNs - routeEndNs) / 1000, generation, performanceMode)
    }

    private fun playLoop(trace: CallAudioTrace) {
        val buf = ShortArray(FRAME_SAMPLES)
        val silence = ShortArray(FRAME_SAMPLES)
        val frameNs = FRAME_MS * 1_000_000L
        var previousPullNs = System.nanoTime()
        var firstWrite = true
        val snapshot = LongArray(8)
        if (!NativeLib.callAudioClock(callId, System.nanoTime() / 1000, snapshot)) {
            reportFailure(callId, "playback clock unavailable")
            return
        }
        val epoch = snapshot[0]
        val playbackClock = CallPlaybackClock()
        var clockMode = ""
        var outputUs = -1L
        var clockFailed = false
        val diagnostic = LongArray(8)
        val row = LongArray(CallAudioTrace.WIDTH)
        while (active) {
            if (outputRecovery.deviceId != confirmedDeviceId) replacePlaybackOutput(epoch, "route")
            if (!active) return
            val pullAtNs = System.nanoTime()
            val pullGapExcessUs = ((pullAtNs - previousPullNs - frameNs) / 1000).coerceAtLeast(0)
            previousPullNs = pullAtNs
            val n = NativeLib.callAudioPull(callId, buf)
            val pullEndNs = System.nanoTime()
            val pullUs = (pullEndNs - pullAtNs) / 1000
            trace.enabled = NativeLib.callAudioDiagnostics(callId, diagnostic)
            val diagnosticUs = (System.nanoTime() - pullEndNs) / 1000
            if (n !in 0..FRAME_SAMPLES) {
                reportFailure(callId, "callAudioPull=$n expected=0..$FRAME_SAMPLES")
                return
            }
            if (n > 0) buf.copyInto(silence, 0, 0, n)
            if (n < FRAME_SAMPLES) silence.fill(0, n, FRAME_SAMPLES)
            var offset = 0
            var blockedAt = 0L
            var zeroWrites = 0L
            var observedPlayed = -1L
            val writeAtNs = System.nanoTime()
            var writeCallNs = 0L
            var headCallNs = 0L
            var progressCallNs = 0L
            while (active && offset < FRAME_SAMPLES) {
                if (outputRecovery.deviceId != confirmedDeviceId) replacePlaybackOutput(epoch, "route")
                val track = audioTrack ?: return
                var operationNs = System.nanoTime()
                val written = track.write(silence, offset, FRAME_SAMPLES - offset)
                writeCallNs += System.nanoTime() - operationNs
                if (written < 0) {
                    if (written == -899 || written == AudioTrack.ERROR_DEAD_OBJECT) {
                        LogManager.addLog("WARN", "CallAudio", "output lost backend=${track.name} result=$written offset=$offset")
                        Thread.sleep(100) // Даём AudioManager доставить подтверждение нового маршрута.
                        if (!active) return
                        val changed = outputRecovery.route(confirmedDeviceId)
                        if (!changed && !outputRecovery.failed()) {
                            reportFailure(callId, "repeated output failure device=$confirmedDeviceId result=$written"); return
                        }
                        replacePlaybackOutput(epoch, "disconnected")
                        blockedAt = 0L
                        continue // Незаписанная часть текущего блока остаётся у worker.
                    }
                    reportFailure(callId, "${track.name}.write=$written offset=$offset state=${track.playState}"); return
                }
                if (written == 0) {
                    zeroWrites++
                    val now = SystemClock.elapsedRealtime()
                    if (blockedAt == 0L) blockedAt = now
                    if (now - blockedAt > 2000) {
                        reportFailure(callId, "AudioTrack stalled for ${now - blockedAt}ms offset=$offset state=${track.playState}")
                        return
                    }
                    Thread.sleep(2)
                    continue
                } else {
                    blockedAt = 0L
                    offset += written
                    playFramesWritten += written
                }
                val nowUs = System.nanoTime() / 1000
                operationNs = System.nanoTime()
                val position = track.playbackHeadPosition
                headCallNs += System.nanoTime() - operationNs
                val played = outputPosition.played(position, playFramesWritten)
                if (played != null) {
                    observedPlayed = played
                    playFramesPlayed = played
                    operationNs = System.nanoTime()
                    val device = trace.device
                    val generation = routeGeneration
                    val anchor = playbackClock.update(playFramesWritten, played, nowUs, generation,
                        device.generation, device.stampFrame, device.stampNs / 1000).takeIf { generation == routeGeneration }
                    outputUs = if (anchor?.measured == true) (anchor.atUs - nowUs).coerceAtLeast(0) else -1
                    outputSample = OutputSample(nowUs, generation, CallOutputLatency(
                        ((playFramesWritten - played) * 1000 / SAMPLE_RATE).toInt(),
                        if (outputUs >= 0) (outputUs / 1000).toInt() else -1))
                    if (anchor != null) NativeLib.callAudioProgress(callId, epoch, anchor.position, anchor.atUs, nowUs, anchor.measured)
                    progressCallNs += System.nanoTime() - operationNs
                    val mode = if (anchor == null) "unavailable" else if (anchor.measured) "timestamp" else "head-estimate"
                    if (mode != clockMode) {
                        LogManager.addLog(if (mode == "unavailable") "WARN" else "INFO", "CallAudio",
                            "playback clock id=$callId mode=$mode routeGeneration=$generation outputUs=$outputUs")
                        clockMode = mode
                    }
                } else if (!clockFailed) {
                    clockFailed = true
                    playFramesPlayed = -1
                    outputUs = -1; outputSample = null
                    // Отрицательная позиция немедленно инвалидирует снимок в native.
                    NativeLib.callAudioProgress(callId, epoch, -1, nowUs, nowUs, false)
                    LogManager.addLog("ERROR", "CallAudio",
                        "playback clock invalid id=$callId epoch=$epoch written=$playFramesWritten; audio continues")
                }
            }
            val writeSpanUs = (System.nanoTime() - writeAtNs) / 1000
            if (trace.enabled && active) {
                val device = trace.device
                val nowUs = System.nanoTime() / 1000
                device.copyTo(row)
                row[0] = nowUs
                row[3] = playFramesWritten; row[4] = observedPlayed
                row[5] = pullUs; row[6] = writeCallNs / 1000; row[7] = pullGapExcessUs
                row[8] = zeroWrites
                diagnostic.copyInto(row, 16); row[24] = offset.toLong()
                row[25] = headCallNs / 1000; row[26] = progressCallNs / 1000; row[27] = writeSpanUs
                row[28] = diagnosticUs; row[29] = nowUs - pullAtNs / 1000
                row[35] = outputUs
                trace.record(row)
            }

            if (firstWrite && active) {
                firstWrite = false
                LogManager.addLog("INFO", "CallAudio",
                    "first playback write id=$callId totalMs=${SystemClock.elapsedRealtime() - startElapsedMs} captureReady=$captureReady")
            }

            // Темп задаёт освобождение AudioTrack. До полной записи блока следующий PCM не извлекаем:
            // запас остаётся в адаптивном JB, а вне устройства ожидает не более одного блока (20 мс).
        }
    }

    /** Reporter управляет устройством независимо от уровня debug; playback не берёт trackLock. */
    private fun adjustPlaybackBuffer(track: CallAudioOutput) {
        trackLock.withLock {
            if (!active || audioTrack !== track) return
            val policy = playbackBuffer ?: return
            try {
                val change = policy.sample(SystemClock.elapsedRealtime(), track.underrunCount, playFramesPlayed) ?: return
                if (change.frames != track.bufferSizeInFrames) {
                    configurePlaybackBuffer(track, change.reason, change)
                } else {
                    LogManager.addLog("WARN", "CallAudio",
                        "playback buffering id=$callId reason=${change.reason} actualFrames=${change.frames} underruns=${change.underruns}")
                }
            } catch (e: Exception) {
                playbackBuffer = null
                LogManager.addLog("ERROR", "CallAudio", "playback buffer monitor disabled id=$callId: ${e.message}")
            }
        }
    }

    /** Под trackLock, вне playback-loop; уменьшение дренирует очередь без flush и потери PCM. */
    private fun configurePlaybackBuffer(track: CallAudioOutput, reason: String, change: CallPlaybackBuffer.Change? = null) {
        try {
            val previous = track.bufferSizeInFrames
            val requested = change?.frames ?: if (track.fast) FRAME_SAMPLES else FRAME_SAMPLES * 2
            val actual = track.setBufferSizeInFrames(requested)
            check(actual > 0) { "setBufferSizeInFrames($requested)=$actual" }
            val start = if (Build.VERSION.SDK_INT >= 31) track.setStartThresholdInFrames(actual) else actual
            check(start > 0) { "setStartThresholdInFrames($actual)=$start" }
            val capacity = track.bufferCapacityInFrames
            val now = SystemClock.elapsedRealtime()
            if (change == null) {
                playbackBuffer = CallPlaybackBuffer(FRAME_SAMPLES).also { it.reset(now, actual, capacity, track.underrunCount) }
            } else {
                playbackBuffer?.applied(now, change, actual, capacity)
            }
            val performance = track.performanceMode
            val level = if (actual == requested && (change == null || change.underruns == 0) &&
                performance == AudioTrack.PERFORMANCE_MODE_LOW_LATENCY) "INFO" else "WARN"
            LogManager.addLog(level, "CallAudio",
                "playback buffering id=$callId backend=${track.name} reason=$reason requestedFrames=$requested actualFrames=$actual " +
                    "previousFrames=$previous startFrames=$start capacityFrames=$capacity underruns=${change?.underruns ?: 0} " +
                    "performanceRequested=${AudioTrack.PERFORMANCE_MODE_LOW_LATENCY} performanceActual=$performance pacing=device")
            if (playReady && (previous != actual || change == null)) {
                // Очередь сначала принимает новый размер; устаревшая задача не затронет следующий звонок.
                if (!mainHandler.post {
                    if (active && audioTrack === track) {
                        mainHandler.removeCallbacks(aecDelayRunnable)
                        scheduleAecDelay(1_000)
                    }
                }) LogManager.addLog("ERROR", "CallAudio", "buffer AEC callback rejected id=$callId")
            }
        } catch (e: Exception) {
            playbackBuffer = null
            LogManager.addLog("WARN", "CallAudio",
                "playback buffer adaptation disabled id=$callId reason=$reason: ${e.message}")
        }
    }

    /** AEC включается только на громкой связи (спикерфон). */
    fun syncAecForRoute(route: AudioRoute) {
        val on = route == AudioRoute.SPEAKER
        LogManager.addLog("INFO", "CallAudio", "AEC ${if (on) "on" else "off"} (route=$route)")
        NativeLib.callAudioSetAec(on)
    }

    private fun scheduleAecDelay(delayMs: Long) {
        if (!mainHandler.postDelayed(aecDelayRunnable, delayMs)) {
            LogManager.addLog("ERROR", "CallAudio", "AEC timer rejected id=$callId delayMs=$delayMs")
        }
    }

    /** Замер задержки рендер→захват после старта, смены выхода или размера его очереди. */
    private fun measureAecDelay() {
        withDeviceLock(recordLock) {
            if (!active || !captureReady || !playReady) return@withDeviceLock
            val rec = audioRecord ?: return@withDeviceLock
            try {
                val output = outputLatency(callId)
                val rTs = AudioTimestamp()
                val recOk = rec.getTimestamp(rTs, AudioTimestamp.TIMEBASE_MONOTONIC) == AudioRecord.SUCCESS
                val inputFrames = if (recOk) callCapturePendingFrames(capFramesRead, rTs.framePosition, rTs.nanoTime, System.nanoTime()) else null
                if (output.outputMs < 0 || inputFrames == null) {
                    LogManager.addLog("WARN", "CallAudio", "getTimestamp unavailable; AEC delay = $AEC_DEFAULT_DELAY_FRAMES frames")
                    NativeLib.callAudioSetAecDelay(AEC_DEFAULT_DELAY_FRAMES)
                    return@withDeviceLock
                }
                // output уже включает HAL и локальную очередь; локальный frame counter нового выхода здесь не используется.
                val trackLatency = output.outputMs * 48L
                val recLatency = inputFrames
                val total = trackLatency + recLatency + AEC_ACOUSTIC_SAMPLES
                val delayFrames = ((total + FRAME_SAMPLES - 1) / FRAME_SAMPLES).toInt().coerceIn(1, 20)
                LogManager.addLog("INFO", "CallAudio",
                    "AEC delay measured: trackLat=$trackLatency recLat=$recLatency total=$total -> $delayFrames frames")
                NativeLib.callAudioSetAecDelay(delayFrames)
            } catch (e: Exception) {
                LogManager.addLog("WARN", "CallAudio", "AEC delay measure failed: ${e.message}")
                try { NativeLib.callAudioSetAecDelay(AEC_DEFAULT_DELAY_FRAMES) }
                catch (fallback: Exception) { LogManager.addLog("ERROR", "CallAudio", "AEC fallback failed id=$callId: $fallback") }
            }
        }
    }

    private fun applyTrackOutputDevice(dev: AudioDeviceInfo?) {
        withDeviceLock(trackLock) {
            val track = audioTrack ?: return@withDeviceLock
            if (!active || router.currentOutputDevice != dev) return@withDeviceLock
            val startedAt = SystemClock.elapsedRealtime()
            try {
                val ok = track.setPreferredDevice(dev)
                LogManager.addLog(if (ok) "INFO" else "WARN", "CallAudio",
                    "track preferred device type=${dev?.type ?: "default"} accepted=$ok totalMs=${SystemClock.elapsedRealtime() - startedAt}")
            } catch (e: Exception) {
                LogManager.addLog("WARN", "CallAudio", "setPreferredDevice failed: ${e.message}")
            }
        }
    }
}
