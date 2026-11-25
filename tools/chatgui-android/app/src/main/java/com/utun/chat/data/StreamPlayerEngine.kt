package com.utun.chat.data

import android.os.Handler
import android.os.Looper
import android.view.Surface
import com.utun.chat.R
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale

/** Audio worker читает независимые треки из RS-кольца и готовит PCM; MediaCodec работает отдельно.
 * Общий native controller убирает избыточный запас ускорением, AudioTrack задаёт часы каждого источника.
 * Временная нехватка сохраняет decoder references; GAP/вытеснение начинают новую точку KEY.
 * Worker сохраняет владельцев и повторяет неудавшийся cleanup; Surface callbacks выполняются после SDK/native ACK.
 */
class StreamPlayerEngine(private val group: String, private val source: String, private val epoch: Long) {
    companion object {
        @Volatile var lowLatency = false // App-scoped DataStore observer; worker only reads.
        private val main = Handler(Looper.getMainLooper())
        private val players = mutableSetOf<StreamPlayerEngine>() // Только main thread.
        private val mutableViewing = MutableStateFlow(false)
        val viewing: StateFlow<Boolean> = mutableViewing

        /** Main: запросить stop всех viewers; незавершённый cleanup запрещает native stop и допускает повтор. */
        fun stopForCore() {
            check(Looper.myLooper() == Looper.getMainLooper()) { "Viewer core teardown requires main" }
            players.toList().forEach { it.stop() }
            players.removeAll { player -> synchronized(player.releaseLock) { player.released } && player.worker?.isAlive != true }
            mutableViewing.value = players.isNotEmpty()
            check(players.isEmpty()) {
                LogManager.addLog("ERROR", "stream", "core teardown blocked by viewers count=${players.size}; cleanup retained")
                "Stream viewers cleanup pending"
            }
            StreamAudioMixer.requireReleased()
        }

        /** Закрыть просмотр и дождаться освобождения AudioTrack/decoder вне main thread. */
        suspend fun stopForCall() {
            val stopped = players.toList()
            for (player in stopped) {
                player.mutableStatus.value = R.string.stream_ended
                player.stop()
            }
            withContext(Dispatchers.IO) {
                for (player in stopped) {
                    player.worker?.join()
                    check(player.worker?.isAlive != true) {
                        LogManager.addLog("ERROR", "stream", "viewer cleanup pending for call id=${player.id}; owner retained")
                        "Viewer cleanup pending id=${player.id}"
                    }
                }
                StreamAudioMixer.awaitIdle()
            }
            players.removeAll(stopped.toSet())
            mutableViewing.value = players.isNotEmpty()
            LogManager.addLog("INFO", "stream", "viewers released for call count=${stopped.size}")
        }
    }
    private val mutableAspect = MutableStateFlow(0f) // Ориентация неизвестна до первого KEY с форматом.
    val aspect: StateFlow<Float> = mutableAspect
    private val mutableStatus = MutableStateFlow(R.string.stream_connecting)
    val status: StateFlow<Int> = mutableStatus
    private val mutableTelemetry = MutableStateFlow(StreamPlaybackTelemetry())
    val telemetry: StateFlow<StreamPlaybackTelemetry> = mutableTelemetry
    private val id = StreamController.nextId()
    private var requestedAtUs = 0L
    @Volatile private var running = false
    private var stopped = false // Main: stop до start также запрещает запуск этого одноразового viewer.
    @Volatile var drainTimeoutMs = 2000L
        private set
    private var worker: Thread? = null
    private val releaseLock = Any()
    private var released = false
    private val releaseCallbacks = mutableListOf<() -> Unit>()

    fun start(surface: Surface) {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Viewer start requires main" }
        if (stopped) { LogManager.addLog("WARN", "stream", "viewer start after stop ignored id=$id"); return }
        if (worker != null) return
        if (CallController.blocksMedia()) {
            LogManager.addLog("WARN", "stream", "viewer blocked by call id=$id")
            mutableStatus.value = R.string.stream_ended
            return
        }
        players.add(this); mutableViewing.value = true
        requestedAtUs = System.nanoTime() / 1000
        LogManager.addLog("INFO", "stream", "android viewer requested id=$id source=$source " +
            "epoch=${epoch.toULong().toString(16)}")
        running = true
        worker = Thread({
            try { if (running) play(surface) }
            finally {
                var delay = 250L
                while (true) {
                    val callbacks = synchronized(releaseLock) {
                        if (releaseCallbacks.isEmpty()) { released = true; null } else releaseCallbacks.toList()
                    } ?: break
                    for (callback in callbacks) try {
                        callback(); synchronized(releaseLock) { releaseCallbacks.remove(callback) }
                    } catch (e: Exception) { LogManager.addLog("ERROR", "stream", "viewer Surface release failed id=$id; retained: $e") }
                    if (synchronized(releaseLock) { releaseCallbacks.isNotEmpty() }) {
                        try { Thread.sleep(delay) } catch (e: InterruptedException) {
                            LogManager.addLog("WARN", "stream", "viewer Surface cleanup wait interrupted id=$id; retrying")
                        }
                        delay = (delay * 2).coerceAtMost(30_000)
                    }
                }
                if (!main.post { players.remove(this); mutableViewing.value = players.isNotEmpty() })
                    LogManager.addLog("ERROR", "stream", "viewer completion post rejected id=$id; registry retains owner")
            }
        }, "stream-audio-$id")
        try { worker!!.start() } catch (e: Exception) {
            running = false; stopped = true
            synchronized(releaseLock) { released = true }
            players.remove(this); mutableViewing.value = players.isNotEmpty(); mutableStatus.value = R.string.stream_failed
            LogManager.addLog("ERROR", "stream", "viewer worker start failed id=$id; no native owners started: $e")
        }
    }
    /** Surface/SurfaceTexture освобождаются callback только после остановки decoder и native DSP. */
    fun stop(onReleased: (() -> Unit)? = null) {
        check(Looper.myLooper() == Looper.getMainLooper()) { "Viewer stop requires main" }
        stopped = true
        running = false
        try { NativeLib.streamStop(id) } catch (e: Exception) {
            LogManager.addLog("ERROR", "stream", "viewer stop request failed id=$id; worker retains cleanup: $e")
        }
        if (onReleased != null) {
            val immediate = synchronized(releaseLock) {
                if (worker == null || released) true else { releaseCallbacks.add(onReleased); false }
            }
            if (immediate) try { onReleased() } catch (e: Exception) {
                LogManager.addLog("ERROR", "stream", "late viewer release callback failed id=$id; caller retains ownership: $e")
                throw e
            }
        }
    }

    private fun play(surface: Surface) {
        var playback: StreamMixedPlayback? = null
        var video: StreamVideoPlayback? = null
        var waitingKey = true
        var audioEnabled = false
        var ended = false
        var videoInputEnded = false
        var audioInputEnded = false
        val records = StreamReadQueue()
        var signal = 0L
        var batches = 0L
        var batchMax = 0
        var videoRevision = 0L
        var position = -1L
        var sourceFinished = false
        var ready = -1L
        var readyTogether = -1L
        var telemetryAtUs = 0L
        var needsAudio = true
        var audioInputs = 0L
        var lastAudioPts = -1L
        var generation = 0L
        var playbackStarted = false
        var endUntil = 0L
        var reported = System.nanoTime() / 1000
        var shownBase = 0L
        var discardedBase = 0L
        var underrunBase = 0L
        var silenceBase = 0L
        var workMaxUs = 0L
        var clockState: LongArray? = null

        fun resetVideo() {
            video?.reset(); waitingKey = true
            shownBase = 0; discardedBase = 0
            mutableStatus.value = R.string.stream_waiting_key
        }
        fun resetTimeline(request: Boolean) {
            playback?.close() // При отказе ссылка остаётся до общего cleanup; новую шкалу не открываем.
            playback = null; position = -1; readyTogether = -1; generation = 0; needsAudio = true; clockState = null; playbackStarted = false
            ready = -1; audioEnabled = false
            mutableTelemetry.value = StreamPlaybackTelemetry()
            underrunBase = 0; silenceBase = 0
            // GAP уже удалил старые записи в StreamReadQueue.add; хвост его batch относится к новой шкале.
            if (request) records.clear()
            ended = false; videoInputEnded = false; audioInputEnded = false; endUntil = 0
            resetVideo()
            if (request) check(NativeLib.streamResync(id)) { "Media resync refused" }
        }
        fun observeStatus(meta: LongArray?) {
            if (meta == null || meta[5] and 1 == 0L) return
            sourceFinished = meta[5] and 4 != 0L
            val enabled = meta[5] and 2 != 0L
            if (playback != null && audioEnabled != enabled) {
                LogManager.addLog("INFO", "stream", "playback audio changed id=$id previous=$audioEnabled enabled=$enabled " +
                    "PTS=$position videoUs=${meta[1]} audioUs=${meta[2]}; resync playback clock")
                resetTimeline(true)
                return
            }
            ready = if (!enabled) meta[1] else if (meta[5] and 16 == 0L) meta[2] else maxOf(meta[1], meta[2])
            readyTogether = if (meta[5] and 16 != 0L) meta[1] else meta[0]
            if (playback != null) return
            audioEnabled = enabled
            playback = StreamMixedPlayback(id, meta[3], audioEnabled) { NativeLib.streamSignalNotify(signal) }
            playback!!.start()
            position = meta[3]; generation = meta[4]
            LogManager.addLog("INFO", "stream", "playback starts id=$id PTS=$position generation=$generation fixedPrefill=0 " +
                "elapsedUs=${System.nanoTime() / 1000 - requestedAtUs} readyUs=$ready videoUs=${meta[1]} audioUs=${meta[2]} " +
                "initialReserveUs=${ready - position} audio=$audioEnabled")
        }
        try {
            check(NativeLib.streamOpen(id, group, source, audio = true, epoch = epoch)) { "Viewer open refused" }
            signal = NativeLib.streamSignalCreate(id).also { check(it != 0L) { "Viewer notification initialization failed" } }
            video = StreamVideoPlayback(id, surface, requestedAtUs,
                wake = { NativeLib.streamSignalNotify(signal) }, onRendered = { if (running) mutableStatus.value = R.string.stream_live })
            video.start()
            LogManager.addLog("INFO", "stream", "viewer audio worker started id=$id preparedPcmMs=${if (lowLatency) 250 else 750} audioAheadUs=${if (lowLatency) 500000 else 1000000}")

            while (running && surface.isValid) {
                check(NativeLib.isReady()) { "Core stopped" }
                video.failure?.let { throw it }
                val workStarted = System.nanoTime() / 1000
                observeStatus(NativeLib.streamStatus(id))
                drainTimeoutMs = ((ready - position) / 1000 + 2000).coerceIn(2000, 30000)
                val deviceLead = playback?.leadUs ?: 0
                var consumed = 0
                for (work in 0 until 32) {
                    if (!running) break
                    needsAudio = !audioInputEnded && (playback?.needsInput ?: true)
                    val audioAhead = if (!audioInputEnded && (!audioEnabled || needsAudio) && records.audios == 0) deviceLead + (if (lowLatency) 500000 else 1000000) else -1L
                    val videoBudget = if (!videoInputEnded && video.wantsInput && records.videos == 0) 1 else 0
                    val batch = if (ended) null else NativeLib.streamPull(id, position, videoBudget, audioAhead, 16)
                    if (batch != null) { batches++; batchMax = maxOf(batchMax, records.add(batch)) }
                    val raw = records.take(!videoInputEnded && video.wantsInput, !audioEnabled || needsAudio) ?: break
                    consumed++
                    check(raw.size >= 28)
                    val b = ByteBuffer.wrap(raw).order(ByteOrder.BIG_ENDIAN)
                    val kind = b.get().toInt(); val flags = b.get().toInt() and 255
                    val width = b.short.toInt() and 65535; val height = b.short.toInt() and 65535
                    val broadcastRotation = b.short.toInt() and 65535; val pts = b.long; val size = b.int
                    val revision = b.int.toLong() and 0xffffffffL
                    val codecId = b.get().toInt() and 255
                    check(b.get() == 0.toByte() && b.get() == 0.toByte() && b.get() == 0.toByte())
                    check(size == b.remaining())
                    if (kind in 1..2 && playback == null) observeStatus(NativeLib.streamStatus(id))
                    when (kind) {
                        1 -> {
                            if (revision != videoRevision) {
                                check(revision > videoRevision); resetVideo(); videoRevision = revision
                            }
                            val key = flags and 1 != 0
                            if (!waitingKey || key) {
                                val bytes = ByteArray(size).also { b.get(it) }
                                val codec = StreamVideoCodec.fromId(codecId)
                                val access = if (key) codec.unpackKey(bytes) else byteArrayOf() to bytes
                                if (waitingKey) {
                                    waitingKey = false
                                    mutableAspect.value = if (broadcastRotation % 180 == 0) width.toFloat() / height else height.toFloat() / width
                                    mutableStatus.value = R.string.stream_waiting_decoder
                                    LogManager.addLog("INFO", "video", "viewer KEY accepted id=$id revision=$revision PTS=$pts bytes=$size " +
                                        "elapsedUs=${System.nanoTime() / 1000 - requestedAtUs} readyAheadUs=${ready - pts}")
                                }
                                video.offer(StreamVideoPlayback.Frame(pts, key, width, height, broadcastRotation, revision, codec, access.first, access.second))
                            }
                        }
                        2 -> if (audioEnabled && playback != null) {
                            audioInputs++; lastAudioPts = pts
                            val result = playback!!.feed(pts, ByteArray(size).also { b.get(it) })
                            if (result == -2) resetTimeline(true) else check(result >= 0) { "Opus feed failed=$result" }
                        }
                        3 -> { LogManager.addLog("INFO", "stream", "playback GAP id=$id"); resetTimeline(false) }
                        4 -> Unit
                        5, 7, 8 -> {
                            if (kind != 8 && !videoInputEnded) { videoInputEnded = true; video.finish() }
                            if (kind != 7 && !audioInputEnded) { audioInputEnded = true; playback?.finish() }
                            if (kind == 5) ended = true
                            LogManager.addLog("INFO", "stream", "viewer track end id=$id kind=$kind videoEnd=$videoInputEnded " +
                                "audioEnd=$audioInputEnded pending=${records.size}")
                        }
                        6 -> {
                            mutableStatus.value = when (flags) {
                                1 -> R.string.stream_group_closed; 2 -> R.string.chat_service_stopped
                                3 -> R.string.stream_source_changed; 4 -> R.string.stream_member_removed
                                7 -> R.string.stream_capacity_exceeded
                                6 -> R.string.stream_source_offline
                                else -> R.string.stream_failed
                            }
                            LogManager.addLog("WARN", "stream", "native viewer closed id=$id reason=$flags"); running = false
                            break
                        }
                        else -> error("Unknown media event=$kind")
                    }
                }
                if (!running) break
                // Ядро перезапрашивает DATA после 5с без прогресса; большой KEY может собираться дольше.
                val decoded = video.state
                if (!playbackStarted && decoded.firstOutputPts != null) playbackStarted = true
                if (playbackStarted && playback != null && ready >= 0) {
                    clockState = playback!!.update(System.nanoTime() / 1000,
                        if (ended) ready.coerceAtLeast(position) else ready,
                        maxOf(40000, playback!!.leadUs + 20000), sourceFinished)
                    check(clockState != null) { "Playback timeline failed" }
                    position = clockState!![0]; needsAudio = clockState!![5] != 0L
                }
                check((playback?.prepare() ?: 0) >= 0) { "PCM preparation failed" }
                video.update(position, ready, playback?.leadUs ?: 0)
                workMaxUs = maxOf(workMaxUs, System.nanoTime() / 1000 - workStarted)
                if (ended && decoded.ended && playback?.empty != false
                    && playback?.drained != false) {
                    if (endUntil == 0L) endUntil = System.nanoTime() + 100_000_000
                    if (System.nanoTime() >= endUntil) { mutableStatus.value = R.string.stream_ended; break }
                }
                val reportNow = System.nanoTime() / 1000
                if (reportNow - telemetryAtUs >= 200_000) {
                    telemetryAtUs = reportNow
                    val reserve = if (readyTogether >= 0 && position >= 0) (readyTogether - position).coerceAtLeast(0) else null
                    val tempo = if (playbackStarted && clockState?.get(4) == 0L) clockState?.get(3)?.toInt() ?: 1000 else 1000
                    try {
                        NativeLib.streamTelemetry(id)?.let { mutableTelemetry.value = StreamPlaybackTelemetry.fromNative(it, reserve, tempo) }
                    } catch (e: Exception) {
                        LogManager.addLog("WARN", "stream", "viewer telemetry invalid id=$id: ${e.message}")
                    }
                }
                if (reportNow - reported >= 500_000) {
                    fun ms(us: Long?, valid: Boolean = true) =
                        if (us != null && valid) String.format(Locale.ROOT, "%.1f", us / 1000.0) else "na"
                    val audio = playback?.diagnostics
                    val state = clockState
                    val reason = if (state == null) { if (waitingKey) "key" else "opening" }
                        else when (state[8].toInt()) {
                            0 -> "starved"; 1 -> "history"; 2 -> "cooldown"; 3 -> "reserve"; 4 -> "catchup"; else -> "invalid"
                        }
                    LogManager.videoDebug("stream PLAY group=$group source=$source " +
                        "epoch=${epoch.toULong().toString(16)} id=$id dt_ms=${(reportNow - reported) / 1000} generation=$generation " +
                        "TIME{position_ms=${ms(position, position >= 0)} ready_ms=${ms(ready, ready >= 0)} " +
                        "reserve_ms=${ms(ready - position, ready >= 0 && position >= 0)} minimum_ms=${ms(state?.get(2))} " +
                        "guard_ms=${ms(state?.get(7))} tempo=${state?.get(3) ?: 1000} reason=$reason " +
                        "correction_ms=${ms(state?.get(9))} pending_gain_ms=${ms(state?.get(10))}} " +
                        "VIDEO{shown=${(decoded.shown - shownBase).coerceAtLeast(0)} drop=${(decoded.discarded - discardedBase).coerceAtLeast(0)} " +
                        "pts_ms=${ms(decoded.shownPts, decoded.shownPts >= 0)} " +
                        "av_ms=${ms(decoded.shownPts - position, decoded.shownPts >= 0 && position >= 0)} " +
                        "input=${records.videos} inflight=${decoded.inflight} decoder_lead_ms=${ms(decoded.decoderLeadUs, playbackStarted)} " +
                        "wait_key=$waitingKey} AUDIO{enabled=$audioEnabled pcm_ms=${ms(audio?.pcmFrames?.toLong()?.times(1000)?.div(48))} " +
                        "device_ms=${ms(playback?.leadUs, audioEnabled && playbackStarted)} " +
                        "underruns=${((audio?.underruns ?: 0) - underrunBase).coerceAtLeast(0)} " +
                        "silence_frames=${((audio?.silenceFrames ?: 0) - silenceBase).coerceAtLeast(0)}} " +
                        "waiting=${state?.get(4) ?: 0} ended=$ended")
                    LogManager.videoTrace("android audio worker id=$id audioInputs=$audioInputs audioPTS=$lastAudioPts " +
                        "needsAudio=$needsAudio ${playback?.description} workMaxUs=$workMaxUs batches=$batches " +
                        "batchMax=$batchMax pending=${records.size} videoEnd=$videoInputEnded audioEnd=$audioInputEnded")
                    reported = reportNow; shownBase = decoded.shown; discardedBase = decoded.discarded
                    underrunBase = audio?.underruns ?: 0; silenceBase = audio?.silenceFrames ?: 0
                    batches = 0; batchMax = 0; workMaxUs = 0
                }
                // PCM consumption, media и video state будят worker; без аудио часы идут по wall clock.
                if (running && consumed < 32) {
                    check(NativeLib.streamSignalWait(signal, if (playbackStarted && !audioEnabled) 5 else 50) >= 0) {
                        "Viewer notification wait failed"
                    }
                }
            }
        } catch (e: Exception) {
            if (running) {
                mutableStatus.value = if (e is StreamVideoPlayback.UnsupportedCodec) R.string.stream_decoder_unsupported else R.string.stream_failed
                LogManager.addLog("ERROR", "stream", "viewer failed id=$id: ${e.message}")
            }
        } finally {
            running = false
            var nativeStopped = false
            var delay = 250L
            if (Thread.interrupted()) LogManager.addLog("WARN", "stream", "viewer interrupted before cleanup id=$id; finishing release")
            while (!nativeStopped || video != null || playback != null || signal != 0L) {
                if (!nativeStopped) try { NativeLib.streamStop(id); nativeStopped = true } catch (e: Exception) {
                    LogManager.addLog("ERROR", "stream", "viewer native stop failed id=$id; retained: $e")
                }
                try { video?.close(); video = null } catch (e: Exception) {
                    LogManager.addLog("ERROR", "video", "viewer decoder cleanup failed id=$id; retained: $e")
                }
                try { playback?.close(); playback = null } catch (e: Exception) {
                    LogManager.addLog("ERROR", "stream", "viewer audio cleanup failed id=$id; retained: $e")
                }
                if (nativeStopped && video == null && playback == null && signal != 0L) try {
                    NativeLib.streamSignalClose(signal); signal = 0
                } catch (e: Exception) { LogManager.addLog("ERROR", "stream", "viewer signal close failed id=$id; retained: $e") }
                if (!nativeStopped || video != null || playback != null || signal != 0L) {
                    LogManager.addLog("DEBUG", "stream", "viewer cleanup pending id=$id native=$nativeStopped " +
                        "video=${video != null} audio=${playback != null} signal=$signal delayMs=$delay")
                    try { Thread.sleep(delay) } catch (e: InterruptedException) {
                        LogManager.addLog("WARN", "stream", "viewer cleanup wait interrupted id=$id; retrying")
                    }
                    delay = (delay * 2).coerceAtMost(30_000)
                }
            }
            mutableTelemetry.value = mutableTelemetry.value.copy(tempoMilli = 1000)
            LogManager.addLog("INFO", "stream", "viewer cleanup confirmed id=$id")
        }
    }
}
