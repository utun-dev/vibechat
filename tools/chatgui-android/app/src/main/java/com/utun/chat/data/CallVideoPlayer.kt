package com.utun.chat.data

import android.media.MediaCodec
import android.os.Handler
import android.os.Looper
import android.view.Surface
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.ArrayDeque
import java.util.concurrent.ConcurrentHashMap

/** Один RX worker, общий абсолютный таймер показа. Новое поколение ждёт drain старого.
 * surface передаёт player владение callback освобождения до его успешного возврата.
 * stop ждёт worker до 250 мс и подтверждает decoder/playout/Surface cleanup; отказ допускает повтор.
 * Освобождение Surface ждёт decoder. После join cleanup может выполнять caller/main retry;
 * callback release должен допускать повтор после исключения. Поздние Surface также получают cleanup.
 * Неподтверждённые ресурсы удерживаются process-ссылкой с повтором через 250 мс..30 с. */
internal class CallVideoPlayer(private val id: Long, private val failure: (String) -> Unit) {
    companion object { private val retained = ConcurrentHashMap.newKeySet<CallVideoPlayer>() }
    private val createdAt = System.nanoTime()
    @Volatile private var running = true
    private class Output(val surface: Surface?, val release: () -> Unit, val detach: Surface? = null)
    private val outputs = ConcurrentLinkedQueue<Output>()
    private val ownedOutputs = ConcurrentLinkedQueue<Output>()
    private var output: Output? = null // RX worker; после join — под монитором player.
    private var decoder: MediaCodec? = null
    private var playout = 0L
    private val cleanupHandler = Handler(Looper.getMainLooper())
    private var cleanupDelay = 250L
    private var cleanupScheduled = false
    private val cleanupRetry = Runnable {
        synchronized(this) { cleanupScheduled = false }
        try { stop() } catch (e: Exception) { LogManager.addLog("WARN", "call", "video RX cleanup retry pending call=$id: $e") }
    }
    private data class Presentation(val surface: Surface, val pts: Long, val at: Long = 0)
    private val presentations = ConcurrentLinkedQueue<Presentation>()
    @Volatile private var awaiting: Presentation? = null
    fun presented(surface: Surface, pts: Long): Boolean {
        val expected = awaiting ?: return true
        if (!running || expected.surface !== surface) return true
        if (expected.pts != pts) return false
        presentations.add(expected.copy(at = System.nanoTime() / 1000)); return true
    }
    private val displayed = MutableStateFlow<CallVideoFormat?>(null)
    val display: StateFlow<CallVideoFormat?> = displayed
    @Volatile private var verified = emptyMap<Int, CallVideoCodec.Companion.Decoder>()
    private val worker = Thread(::loop, "call-video-rx")
    /** Владелец сначала сохраняет player, затем запускает worker; stop допустим также до start. */
    @Synchronized fun start() {
        check(running && worker.state == Thread.State.NEW) {
            LogManager.addLog("ERROR", "call", "video RX start rejected call=$id running=$running state=${worker.state}")
            "Video RX already started/stopped"
        }
        worker.start()
    }
    @Synchronized fun surface(surface: Surface?, release: () -> Unit = {}) {
        val owner = Output(surface, release)
        ownedOutputs.add(owner)
        if (running) outputs.add(owner) else {
            LogManager.addLog("WARN", "call", "video surface received after RX stop call=$id; retained for cleanup")
            scheduleCleanup()
        }
    }
    @Synchronized fun detach(surface: Surface) {
        if (running) outputs.add(Output(null, {}, surface))
    }
    fun decoders(value: Map<Int, CallVideoCodec.Companion.Decoder>) { verified = value }
    /** Caller вне RX: ошибка join/release сохраняет владельцев и блокирует native port close. */
    fun stop() {
        check(Thread.currentThread() !== worker) {
            LogManager.addLog("ERROR", "call", "video RX stop from its own worker call=$id")
            "Video RX cannot join itself"
        }
        synchronized(this) { running = false }
        worker.interrupt()
        try { worker.join(250) } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            LogManager.addLog("ERROR", "call", "video RX join interrupted call=$id: $e")
            synchronized(this) { scheduleCleanup() }
            throw e
        }
        synchronized(this) {
            if (!worker.isAlive) { outputs.clear(); releaseResources() }
            scheduleCleanup()
            check(!worker.isAlive && decoder == null && playout == 0L && ownedOutputs.isEmpty()) {
                val detail = "video RX cleanup pending call=$id worker=${worker.state} decoder=${decoder != null} " +
                    "playout=$playout surfaces=${ownedOutputs.size}"
                LogManager.addLog("ERROR", "call", detail)
                detail
            }
        }
    }

    private fun releaseDecoder(): Boolean {
        val owned = decoder ?: return true
        try { owned.stop() } catch (e: Exception) { LogManager.addLog("WARN", "call", "video decoder stop call=$id: $e") }
        return try { owned.release(); decoder = null; true } catch (e: Exception) {
            LogManager.addLog("ERROR", "call", "video decoder release failed call=$id; retained for retry: $e")
            false
        }
    }

    private fun releaseOutput(owner: Output): Boolean = try {
        owner.release(); ownedOutputs.remove(owner); true
    } catch (e: Exception) {
        LogManager.addLog("ERROR", "call", "video surface release failed call=$id surface=${System.identityHashCode(owner.surface)}; retained: $e")
        false
    }

    /** Только finally RX либо после join: независимые владельцы очищаются даже при ошибке соседнего. */
    private fun releaseResources() {
        releaseDecoder()
        for (owner in ownedOutputs.toList()) {
            if (decoder != null && owner === output) continue
            if (releaseOutput(owner) && owner === output) output = null
        }
        if (decoder == null && output !in ownedOutputs) output = null
        if (playout != 0L) try {
            CallVideoNative.playoutDestroy(playout); playout = 0
        } catch (e: Exception) { LogManager.addLog("ERROR", "call", "video playout destroy failed call=$id; retained: $e") }
    }

    private fun scheduleCleanup() {
        if (!worker.isAlive && decoder == null && playout == 0L && ownedOutputs.isEmpty()) {
            cleanupHandler.removeCallbacks(cleanupRetry); cleanupScheduled = false
            if (retained.remove(this)) LogManager.addLog("INFO", "call", "video RX cleanup confirmed call=$id")
            cleanupDelay = 250
            return
        }
        retained.add(this)
        if (cleanupScheduled) return
        val delay = cleanupDelay
        cleanupScheduled = cleanupHandler.postDelayed(cleanupRetry, delay)
        if (!cleanupScheduled) {
            LogManager.addLog("ERROR", "call", "video RX cleanup retry rejected call=$id; owner retained delayMs=$delay")
        } else {
            cleanupDelay = (cleanupDelay * 2).coerceAtMost(30_000)
            LogManager.addLog("DEBUG", "call", "video RX cleanup scheduled call=$id delayMs=$delay")
        }
    }

    private fun reportFailure(detail: String) {
        LogManager.addLog("ERROR", "call", "video receive call=$id: $detail")
        try { failure(detail) } catch (e: Exception) { LogManager.addLog("ERROR", "call", "video RX failure callback failed call=$id: $e") }
    }

    private fun loop() {
        data class Timing(val pts: Long, val received: Long, val interval: Int)
        data class Picture(val index: Int, val timing: Timing)
        var surface: Surface? = null; var format: CallVideoFormat? = null
        var lastRendered = 0L
        var awaitingPts = -1L
        var pending: ByteArray? = null; val meta = LongArray(13)
        val pictures = ArrayDeque<Picture>(); val inputs = ArrayDeque<Timing>()
        var draining = false; var endSeen = false
        var lastOutput = -1L; var lastProgress = System.nanoTime()
        var decoderPipeline = 0
        var reportedGeneration = 0L; var rejectedGeneration = 0L
        var reportAt = System.nanoTime(); var pulled = 0; var queued = 0; var decoded = 0; var rendered = 0; var late = 0
        var acknowledged = 0; var ackMaxUs = 0L
        var firstInput = true; var firstDecoded = true; var firstPresented = true
        val clock = LongArray(8); val info = MediaCodec.BufferInfo()
        fun close() {
            check(releaseDecoder()) { running = false; "Video decoder cleanup pending" }
            format = null; pictures.clear(); inputs.clear()
            awaitingPts = -1; awaiting = null; presentations.clear()
            draining = false; endSeen = false; lastOutput = -1
        }
        try {
            playout = CallVideoNative.playoutCreate(id)
            check(playout != 0L) { "Video playout allocation failed" }
            while (running) {
                var worked = false; var wakeUs = System.nanoTime() / 1000 + 5000
                try {
                    while (true) {
                        val next = outputs.peek() ?: break
                        if (next.detach != null && output?.surface !== next.detach) { outputs.poll(); continue }
                        close()
                        output?.let { check(releaseOutput(it)) { running = false; "Video surface cleanup pending" } }
                        output = next; surface = next.surface; outputs.poll()
                        CallVideoNative.playoutReset(playout)
                        pending = null; displayed.value = null
                        if (surface != null) CallVideoNative.requestKey(id)
                        LogManager.addLog("INFO", "call", "video output attached=${surface != null} call=$id elapsedMs=${(System.nanoTime() - createdAt) / 1_000_000}")
                    }
                    while (true) {
                        val consumed = presentations.poll() ?: break
                        if (consumed.surface === surface && consumed.pts == awaitingPts) {
                            acknowledged++; ackMaxUs = maxOf(ackMaxUs, (consumed.at * 1000 - lastRendered) / 1000)
                            CallVideoNative.playoutPresented(playout, consumed.pts, consumed.at)
                            if (firstPresented) {
                                firstPresented = false
                                LogManager.addLog("INFO", "call", "video startup call=$id stage=first_presented " +
                                    "elapsedMs=${(consumed.at * 1000 - createdAt) / 1_000_000} pts=${consumed.pts} " +
                                    "surfaceWaitUs=${(consumed.at * 1000 - lastRendered) / 1000}")
                            }
                            awaitingPts = -1; awaiting = null; worked = true
                        }
                    }
                    // Малый output lookahead; основной долг остаётся в encoded FIFO.
                    while (decoder != null && pictures.size + (if (awaitingPts >= 0) 1 else 0) < 3 && !endSeen) {
                        val out = decoder!!.dequeueOutputBuffer(info, 0)
                        if (out >= 0) {
                            worked = true; lastProgress = System.nanoTime()
                            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) endSeen = true
                            if (info.size == 0) decoder!!.releaseOutputBuffer(out, false)
                            else {
                                check(info.presentationTimeUs > lastOutput) { "Video decoder reordered PTS" }
                                format?.let { f -> if (reportedGeneration != f.generation) {
                                    CallVideoNative.decoded(id, f.generation, true); reportedGeneration = f.generation
                                } }
                                val timing = checkNotNull(inputs.pollFirst()) { "Video output without input timing" }
                                check(timing.pts == info.presentationTimeUs) { "Video output timing mismatch" }
                                lastOutput = info.presentationTimeUs; pictures.addLast(Picture(out, timing))
                                if (firstDecoded) {
                                    firstDecoded = false
                                    LogManager.addLog("INFO", "call", "video startup call=$id stage=first_decoded " +
                                        "elapsedMs=${(System.nanoTime() - createdAt) / 1_000_000} pts=$lastOutput format=$format")
                                }
                                decoded++
                            }
                        } else if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                            LogManager.addLog("INFO", "call", "video decoder output call=$id format=${decoder!!.outputFormat}")
                        } else break
                    }
                    val now = System.nanoTime()
                    val valid = NativeLib.callAudioClock(id, now / 1000, clock) && clock[5] != 0L
                    val first = if (awaitingPts < 0) pictures.peekFirst() else null
                    val localQueued = inputs.size + pictures.size + (if (pending != null) 1 else 0) + (if (awaitingPts >= 0) 1 else 0)
                    val decision = CallVideoNative.playoutStep(playout, first?.timing?.pts ?: -1, first?.timing?.received ?: 0,
                        now / 1000, first?.timing?.interval ?: 0, localQueued,
                        1 + (format?.let { f -> verified[f.codec]?.modes?.firstOrNull { it.supports(f) }?.pipeline } ?: 0))
                    if (decision > 0) wakeUs = now / 1000 + decision
                    else if (first != null) {
                        val render = decision == 0L
                        if (render) awaiting = Presentation(checkNotNull(surface), first.timing.pts)
                        decoder!!.releaseOutputBuffer(first.index, render); pictures.removeFirst(); worked = true
                        if (render) {
                            awaitingPts = first.timing.pts
                            rendered++
                            if (displayed.value != format) LogManager.addLog("INFO", "call", "video visible call=$id format=$format " +
                                "elapsedMs=${(now - createdAt) / 1_000_000} pts=${first.timing.pts}")
                            displayed.value = format; lastRendered = now
                        } else late++
                        lastProgress = now
                    }
                    if (endSeen && pictures.isEmpty() && awaitingPts < 0) {
                        check(inputs.isEmpty()) { "Video EOS before all output frames" }
                        LogManager.addLog("DEBUG", "call", "video decoder drained call=$id generation=${format?.generation}")
                        close()
                    }
                    // Ожидание срока output не останавливает подачу input; очередь остаётся ограниченной.
                    val decodedSpace = pictures.size + (if (awaitingPts >= 0) 1 else 0) < 3
                    if (pending == null && inputs.size < 8 && decodedSpace) {
                        pending = CallVideoNative.pull(id, meta)
                        if (pending != null) {
                            pulled++
                            if (firstInput) {
                                firstInput = false
                                LogManager.addLog("INFO", "call", "video startup call=$id stage=first_received " +
                                    "elapsedMs=${(System.nanoTime() - createdAt) / 1_000_000} generation=${meta[0]} pts=${meta[10]} " +
                                    "key=${meta[11] != 0L} surfaceReady=${surface?.isValid == true}")
                            }
                        }
                    }
                    val data = pending
                    if (data != null && surface?.isValid == true && decodedSpace) {
                        val next = CallVideoFormat.from(meta)
                        if (next.generation == rejectedGeneration) { pending = null; continue }
                        val key = meta[11] != 0L
                        if (format != next && decoder != null) {
                            if (!draining) {
                                val input = decoder!!.dequeueInputBuffer(0)
                                if (input >= 0) { decoder!!.queueInputBuffer(input, 0, 0, meta[10], MediaCodec.BUFFER_FLAG_END_OF_STREAM); draining = true }
                            }
                        } else {
                            if (decoder == null) {
                                if (!key) { pending = null; continue }
                                CallVideoCodec.validateKey(data, next)
                                val type = StreamVideoCodec.fromId(next.codec); val config = type.unpackKey(data).first
                                val implementation = checkNotNull(verified[next.codec]?.modes?.firstOrNull { it.supports(next) }) {
                                    "No verified decoder for $next"
                                }
                                decoder = MediaCodec.createByCodecName(implementation.name)
                                decoder!!.configure(CallVideoCodec.decoderFormat(next, decoder!!, config), surface, null, 0)
                                decoder!!.start(); format = next; lastProgress = System.nanoTime()
                                decoderPipeline = implementation.pipeline
                                LogManager.addLog("INFO", "call", "video decoder started call=$id codec=${implementation.name} format=$next")
                            }
                            val index = decoder!!.dequeueInputBuffer(0)
                            if (index >= 0) {
                                val bytes = if (key) {
                                    CallVideoCodec.validateKey(data, next)
                                    val parts = StreamVideoCodec.fromId(next.codec).unpackKey(data); parts.first + parts.second
                                } else data
                                val buffer = checkNotNull(decoder!!.getInputBuffer(index)); check(bytes.size <= buffer.capacity())
                                buffer.put(bytes); decoder!!.queueInputBuffer(index, 0, bytes.size, meta[10], 0)
                                inputs.addLast(Timing(meta[10], meta[12], 1000000 / next.fps)); pending = null; queued++; worked = true
                                if (inputs.size == decoderPipeline + 1) lastProgress = System.nanoTime()
                            }
                        }
                    } else if (data != null && surface == null) pending = null
                    if (decoder != null && awaitingPts < 0 && pictures.isEmpty() && inputs.size > decoderPipeline
                        && now - lastProgress > 2_000_000_000) {
                        LogManager.addLog("WARN", "call", "video decoder stalled call=$id generation=${format?.generation} " +
                            "pending=${inputs.size} pipeline=$decoderPipeline; requesting KEY")
                        close(); CallVideoNative.playoutReset(playout); CallVideoNative.requestKey(id)
                    }
                    if (awaitingPts < 0 && lastRendered != 0L && now - lastRendered >= 5_000_000_000) {
                        displayed.value = null; lastRendered = 0
                        LogManager.addLog("INFO", "call", "video hidden: no rendered frames for 5s call=$id")
                    }
                    if (now - reportAt >= 1_000_000_000) {
                        LogManager.addLog("DEBUG", "call", "video playback call=$id intervalMs=${(now - reportAt) / 1_000_000} " +
                            "pulled=$pulled queued=$queued decoded=$decoded rendered=$rendered skipped=$late inflight=${inputs.size} " +
                            "pending=${pending != null} ready=${pictures.size} totalLocal=$localQueued awaitingSurfacePts=$awaitingPts " +
                            "surfaceWaitMs=${if (awaitingPts >= 0) (now - lastRendered) / 1_000_000 else 0} " +
                            "acknowledged=$acknowledged surfaceAckMaxUs=$ackMaxUs audioValid=$valid audioPts=${clock[4]}")
                        reportAt = now; pulled = 0; queued = 0; decoded = 0; rendered = 0; late = 0
                        acknowledged = 0; ackMaxUs = 0
                    }
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "call", "video receive call=$id: $e")
                    val generation = if (pending != null) meta[0] else format?.generation ?: 0
                    if (generation != 0L) { CallVideoNative.decoded(id, generation, false); rejectedGeneration = generation }
                    close(); CallVideoNative.playoutReset(playout)
                    pending = null; displayed.value = null; reportFailure(e.message ?: "Video decoder failed")
                }
                val waitUs = wakeUs - System.nanoTime() / 1000
                if (!worked && waitUs > 0) Thread.sleep(waitUs / 1000, (waitUs % 1000 * 1000).toInt())
            }
        } catch (e: Exception) {
            if (!running && e is InterruptedException) LogManager.addLog("DEBUG", "call", "video RX wait cancelled call=$id")
            else reportFailure("Video RX stopped: $e")
        } finally {
            synchronized(this) {
                running = false; displayed.value = null; awaiting = null; presentations.clear(); outputs.clear()
                releaseResources()
                scheduleCleanup()
                LogManager.addLog("INFO", "call", "video RX worker exited call=$id decoder=${decoder != null} playout=$playout surfaces=${ownedOutputs.size}")
            }
        }
    }
}
